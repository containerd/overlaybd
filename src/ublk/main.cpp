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

static int read_pidfile(int dev_id, pid_t *pid) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%d.pid", OVERLAYBD_UBLK_RUN_DIR, dev_id);
    FILE *fp = fopen(path, "r");
    if (fp == nullptr)
        return -1;
    long v = 0;
    int n = fscanf(fp, "%ld", &v);
    fclose(fp);
    if (n != 1 || v <= 0)
        return -1;
    *pid = (pid_t)v;
    return 0;
}

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

// del: process == device, so a graceful stop is SIGTERM to the
// daemon, whose signal handler tears the ublk device down before exiting.
static int cmd_del(int dev_id) {
    pid_t pid;
    if (read_pidfile(dev_id, &pid) != 0) {
        fprintf(stderr, "overlaybd-ublk: no pidfile for dev %d under %s\n", dev_id,
                OVERLAYBD_UBLK_RUN_DIR);
        return 1;
    }
    if (pid_is_ublkd(pid)) {
        fprintf(stderr,
                "overlaybd-ublk: /dev/ublkb%d is managed by overlaybd-ublkd "
                "(pid %d); SIGTERM would tear down ALL of its devices.\n"
                "Use the daemon API instead:\n"
                "  curl --unix-socket %s/ublkd.sock -X POST "
                "-d '{\"dev_id\":%d}' http://d/v1/del\n",
                dev_id, (int)pid, OVERLAYBD_UBLK_RUN_DIR, dev_id);
        return 1;
    }
    if (kill(pid, SIGTERM) != 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "overlaybd-ublk: dev %d daemon (pid %d) not running, "
                            "removing stale pidfile\n",
                    dev_id, (int)pid);
            char path[128];
            snprintf(path, sizeof(path), "%s/%d.pid", OVERLAYBD_UBLK_RUN_DIR, dev_id);
            unlink(path);
            return 1;
        }
        fprintf(stderr, "overlaybd-ublk: kill pid %d failed: %s\n", (int)pid,
                strerror(errno));
        return 1;
    }
    // wait for the daemon to finish teardown (device gone when it exits)
    for (int i = 0; i < 300; i++) { // up to 30s
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            printf("/dev/ublkb%d removed\n", dev_id);
            return 0;
        }
        usleep(100 * 1000);
    }
    fprintf(stderr, "overlaybd-ublk: dev %d daemon (pid %d) did not exit in 30s\n", dev_id,
            (int)pid);
    return 1;
}

static int cmd_list() {
    DIR *dir = opendir(OVERLAYBD_UBLK_RUN_DIR);
    if (dir == nullptr)
        return 0; // nothing ever started on this host
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        int dev_id;
        if (sscanf(ent->d_name, "%d.pid", &dev_id) != 1)
            continue;
        pid_t pid;
        if (read_pidfile(dev_id, &pid) != 0)
            continue;
        bool alive = (kill(pid, 0) == 0);
        // best effort: show the image config from the daemon's cmdline
        std::string cmdline;
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
        printf("/dev/ublkb%-4d pid %-8d %-8s %s%s\n", dev_id, (int)pid,
               alive ? "running" : "dead",
               pid_is_ublkd(pid) ? "[ublkd-managed] " : "", cmdline.c_str());
    }
    closedir(dir);
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
        return cmd_del(cmd.del_dev_id);
    case UblkCliCmd::Kind::LIST:
        return cmd_list();
    default:
        return 1;
    }
}
