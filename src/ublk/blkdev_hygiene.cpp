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
#include "blkdev_hygiene.h"

#include <photon/common/alog.h>

#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/fs.h> // BLKFLSBUF
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h> // major/minor
#include <unistd.h>

static std::string dev_path(int dev_id) {
    return "/dev/ublkb" + std::to_string(dev_id);
}

bool ublk_device_is_mounted(int dev_id) {
    struct stat st;
    std::string path = dev_path(dev_id);
    if (stat(path.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) {
        LOG_WARN("cannot stat ` for mount check, assuming mounted", path.c_str());
        return true; // fail safe
    }
    char want[32];
    snprintf(want, sizeof(want), "%u:%u", major(st.st_rdev), minor(st.st_rdev));

    FILE *fp = fopen("/proc/self/mountinfo", "r");
    if (fp == nullptr) {
        LOG_WARN("cannot read mountinfo, assuming ` is mounted", path.c_str());
        return true; // fail safe
    }
    // mountinfo: id parent major:minor root mountpoint ...
    char line[4096];
    bool mounted = false;
    while (fgets(line, sizeof(line), fp) != nullptr) {
        int id, parent;
        char majmin[64];
        if (sscanf(line, "%d %d %63s", &id, &parent, majmin) != 3)
            continue;
        if (strcmp(majmin, want) == 0) {
            mounted = true;
            break;
        }
    }
    fclose(fp);
    return mounted;
}

int ublk_device_flush_buffers(int dev_id) {
    std::string path = dev_path(dev_id);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    int ret = 0;
    // BLKFLSBUF writes back dirty pages and then invalidates the cache
    if (ioctl(fd, BLKFLSBUF, 0) != 0)
        ret = -errno;
    close(fd);
    if (ret != 0)
        LOG_WARN("BLKFLSBUF on ` failed: `", path.c_str(), strerror(-ret));
    return ret;
}
