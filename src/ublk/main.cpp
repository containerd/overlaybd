/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "cli.h"
#include "ublk_device.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// A pidfile under the run dir may belong to a one-device CLI daemon or to
// overlaybd-ublkd (libublksrv writes <dev_id>.pid with the serving process'
// pid either way). The distinction matters: SIGTERM to a CLI daemon removes
// exactly its device, but SIGTERM to ublkd gracefully tears down ALL its
// devices -- `del -n N` must never do that by accident.
static bool pid_is_ublkd(pid_t pid) {
    char path[64], comm[64] = {};
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *fp = fopen(path, "r");
    if (fp == nullptr)
        return false;
    if (fgets(comm, sizeof(comm), fp) == nullptr)
        comm[0] = 0;
    fclose(fp);
    comm[strcspn(comm, "\n")] = 0;
    return strcmp(comm, "overlaybd-ublkd") == 0;
}

// del: drive the kernel directly instead of signaling the
// owner. STOP/DEL go through /dev/ublk-control by dev_id, so orphans (owner
// killed -9) are deletable and no pidfile is needed. A live CLI owner is
// stopped externally: its queue dies, it drains and exits WITHOUT deleting
// (teardown skips DEL when the stop was not its own), then we DEL here.
static void remove_pidfile(int dev_id) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%d.pid", OVERLAYBD_UBLK_RUN_DIR, dev_id);
    unlink(path);
}

// owner pid from the pidfile written by libublksrv. Needed by --force: once
// STOP_DEV has been issued against a device whose owner is frozen, the
// kernel holds that device's control plane and every further ctrl command
// (even GET_DEV_INFO) blocks forever -- so --force must learn the pid
// WITHOUT asking the kernel, kill the owner, and only then touch the
// control plane. Verified the hard way: --force used to hang in
// io_cqring_wait on its very first GET_DEV_INFO.
static pid_t pid_from_pidfile(int dev_id) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%d.pid", OVERLAYBD_UBLK_RUN_DIR, dev_id);
    FILE *fp = fopen(path, "r");
    if (fp == nullptr)
        return -1;
    long v = -1;
    int n = fscanf(fp, "%ld", &v);
    fclose(fp);
    return (n == 1 && v > 0) ? (pid_t)v : -1;
}

static int cmd_del(int dev_id, bool force) {
    if (force) {
        pid_t owner = pid_from_pidfile(dev_id);
        if (owner > 0 && kill(owner, 0) == 0) {
            if (pid_is_ublkd(owner)) {
                fprintf(stderr,
                        "overlaybd-ublk: refusing --force on a device of a live "
                        "overlaybd-ublkd (pid %d); stop the daemon instead\n",
                        (int)owner);
                return 1;
            }
            // SIGCONT first: a SIGSTOPped owner does not act on a queued
            // SIGKILL until it is resumed (observed on a frozen owner)
            kill(owner, SIGCONT);
            kill(owner, SIGKILL);
            int i;
            for (i = 0; i < 100 && kill(owner, 0) == 0; i++)
                usleep(100 * 1000);
            if (i == 100)
                fprintf(stderr, "overlaybd-ublk: owner pid %d survived SIGKILL "
                                "(D state?), trying kernel teardown anyway\n",
                        (int)owner);
            else
                fprintf(stderr, "overlaybd-ublk: killed owner pid %d\n", (int)owner);
            sleep(1); // let the kernel run its owner-death cleanup
        }
    }

    UblkKernelDevInfo info;
    int r = ublk_kernel_get_dev_info(dev_id, info);
    if (r == -ENODEV || r == -ENOENT) {
        fprintf(stderr, "overlaybd-ublk: no ublk device %d in the kernel\n", dev_id);
        remove_pidfile(dev_id); // stale leftovers
        return 1;
    }
    if (r < 0) {
        fprintf(stderr, "overlaybd-ublk: GET_DEV_INFO(%d) failed: %s\n", dev_id,
                strerror(-r));
        return 1;
    }

    pid_t pid = info.owner_pid;
    bool alive = pid > 0 && kill(pid, 0) == 0;
    if (alive && pid_is_ublkd(pid)) {
        fprintf(stderr,
                "overlaybd-ublk: /dev/ublkb%d is managed by overlaybd-ublkd "
                "(pid %d); deleting it here would rip it out under the daemon.\n"
                "Use the daemon API instead:\n"
                "  curl --unix-socket %s/ublkd.sock -X POST "
                "-d '{\"dev_id\":%d}' http://d/v1/del\n",
                dev_id, (int)pid, OVERLAYBD_UBLK_RUN_DIR, dev_id);
        return 1;
    }

    if (alive) {
        // live single-device daemon: external STOP, then wait for its exit
        // (it drains in-flight IO and releases the device references)
        r = ublk_kernel_stop_dev(dev_id);
        if (r < 0 && r != -ENODEV)
            fprintf(stderr, "overlaybd-ublk: STOP_DEV: %s (continuing)\n",
                    strerror(-r));
        int waited;
        for (waited = 0; waited < 300; waited++) { // up to 30s
            if (kill(pid, 0) != 0 && errno == ESRCH)
                break;
            usleep(100 * 1000);
        }
        if (waited == 300) {
            fprintf(stderr,
                    "overlaybd-ublk: owner pid %d did not exit in 30s "
                    "(wedged?). Aborting -- deleting a device with stuck "
                    "IO can hang the machine on backported kernels. "
                    "Retry with --force to SIGKILL the owner first.\n",
                    (int)pid);
            return 1;
        }
    } else {
        // orphan (or just-killed --force owner): references dropped, kernel
        // auto-stopped; STOP may fail depending on state -- best effort
        ublk_kernel_stop_dev(dev_id);
    }

    r = ublk_kernel_del_dev(dev_id);
    if (r < 0 && r != -ENODEV && r != -ENOENT) {
        fprintf(stderr, "overlaybd-ublk: DEL_DEV(%d) failed: %s\n", dev_id,
                strerror(-r));
        return 1;
    }
    remove_pidfile(dev_id);
    printf("/dev/ublkb%d removed\n", dev_id);
    return 0;
}

// list: the KERNEL is the source of truth -- scan /sys/block for
// ublkbN, ask GET_DEV_INFO for the owner, and grade each device:
// running / [ublkd-managed] / ORPHAN (kernel device without a live owner).
// Stale pidfiles without a kernel device are reported for cleanup.
static int cmd_list() {
    bool seen[1024] = {};
    DIR *bd = opendir("/sys/block");
    if (bd != nullptr) {
        struct dirent *ent;
        while ((ent = readdir(bd)) != nullptr) {
            int dev_id;
            if (sscanf(ent->d_name, "ublkb%d", &dev_id) != 1)
                continue;
            if (dev_id >= 0 && dev_id < 1024)
                seen[dev_id] = true;
            UblkKernelDevInfo info;
            if (ublk_kernel_get_dev_info(dev_id, info) != 0)
                continue;
            pid_t pid = info.owner_pid;
            bool alive = pid > 0 && kill(pid, 0) == 0;
            const char *status = !alive              ? "ORPHAN"
                                 : pid_is_ublkd(pid) ? "[ublkd-managed]"
                                                     : "running";
            // best effort: show the image config from the owner's cmdline
            std::string cmdline;
            if (alive) {
                char proc_path[64], buf[4096];
                snprintf(proc_path, sizeof(proc_path), "/proc/%d/cmdline", (int)pid);
                int fd = open(proc_path, O_RDONLY);
                if (fd >= 0) {
                    ssize_t n = read(fd, buf, sizeof(buf) - 1);
                    close(fd);
                    for (ssize_t i = 0; i < n; i++)
                        buf[i] = buf[i] ? buf[i] : ' ';
                    if (n > 0) {
                        buf[n] = 0;
                        cmdline = buf;
                    }
                }
            }
            printf("/dev/ublkb%-4d pid %-8d %-15s %s\n", dev_id, (int)pid, status,
                   cmdline.c_str());
        }
        closedir(bd);
    }
    // Residuals: after an owner dies, this kernel auto-STOPs the device
    // (gendisk gone, invisible in /sys/block) but does NOT free the id --
    // /dev/ublkcN stays behind. Scan for those, they are reclaimable.
    DIR *dd = opendir("/dev");
    if (dd != nullptr) {
        struct dirent *ent;
        while ((ent = readdir(dd)) != nullptr) {
            int dev_id;
            if (sscanf(ent->d_name, "ublkc%d", &dev_id) != 1)
                continue;
            if (dev_id < 0 || dev_id >= 1024 || seen[dev_id])
                continue;
            seen[dev_id] = true;
            UblkKernelDevInfo info;
            if (ublk_kernel_get_dev_info(dev_id, info) != 0)
                continue;
            printf("ublk dev %-6d pid %-8d %-15s (stopped residual, id still "
                   "allocated; reclaim with: del -n %d)\n",
                   dev_id, info.owner_pid, "RESIDUAL", dev_id);
        }
        closedir(dd);
    }
    // pidfiles whose kernel device is gone: stale, worth flagging
    DIR *rd = opendir(OVERLAYBD_UBLK_RUN_DIR);
    if (rd != nullptr) {
        struct dirent *ent;
        while ((ent = readdir(rd)) != nullptr) {
            int dev_id;
            if (sscanf(ent->d_name, "%d.pid", &dev_id) != 1)
                continue;
            if (dev_id >= 0 && dev_id < 1024 && !seen[dev_id])
                printf("(stale pidfile %s/%d.pid: no kernel device)\n",
                       OVERLAYBD_UBLK_RUN_DIR, dev_id);
        }
        closedir(rd);
    }
    return 0;
}

// add: fork the daemon and wait for the sync-ready contract -- the parent
// only exits 0 after the device path arrived over the pipe, and that path
// is the single authoritative stdout output.
static int cmd_add(const UblkDeviceOpts &opts, bool foreground) {
    if (ublk_check_control_dev() != 0)
        return 1;

    if (foreground)
        return run_ublk_device(opts, STDOUT_FILENO);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "overlaybd-ublk: pipe failed: %s\n", strerror(errno));
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "overlaybd-ublk: fork failed: %s\n", strerror(errno));
        return 1;
    }

    if (child == 0) {
        // daemon child: detach from the terminal; logs go to the log file
        // configured in the global service config (alog), not the console
        close(pipefd[0]);
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        exit(run_ublk_device(opts, pipefd[1]));
    }

    // parent: block until the child reports readiness or dies (pipe EOF)
    close(pipefd[1]);
    char buf[256];
    ssize_t n = 0, off = 0;
    while (off < (ssize_t)sizeof(buf) - 1 &&
           (n = read(pipefd[0], buf + off, sizeof(buf) - 1 - off)) > 0) {
        off += n;
        if (memchr(buf, '\n', off) != nullptr)
            break;
    }
    close(pipefd[0]);
    buf[off > 0 ? off : 0] = 0;

    if (off > 0 && strncmp(buf, "/dev/", 5) == 0) {
        fwrite(buf, 1, off, stdout); // e.g. "/dev/ublkb0\n"
        return 0;
    }
    if (off > 4 && strncmp(buf, "ERR:", 4) == 0) {
        // failure reason relayed over the ready pipe: the daemonized child
        // has no stderr and alog may not be file-backed yet at that point
        fprintf(stderr, "overlaybd-ublk: %s", buf + 4);
        return 1;
    }
    fprintf(stderr, "overlaybd-ublk: device failed to start "
                    "(check the overlaybd log for details)\n");
    return 1;
}

int main(int argc, char **argv) {
    UblkCliCmd cmd;
    int ret = ublk_parse_cli(argc, argv, cmd);
    if (cmd.kind == UblkCliCmd::Kind::NONE)
        return ret;

    switch (cmd.kind) {
    case UblkCliCmd::Kind::ADD:
        return cmd_add(cmd.opts, cmd.foreground);
    case UblkCliCmd::Kind::DEL:
        return cmd_del(cmd.del_dev_id, cmd.del_force);
    case UblkCliCmd::Kind::LIST:
        return cmd_list();
    default:
        return 1;
    }
}
