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
#pragma once

#include <string>

// pidfiles (<dev_id>.pid, created by libublksrv) live here; `del` and `list`
// discover overlaybd-ublk daemons through this directory only.
#define OVERLAYBD_UBLK_RUN_DIR "/var/run/overlaybd-ublk"
#define UBLK_CONTROL_DEV "/dev/ublk-control"

struct UblkDeviceOpts {
    std::string image_config_path;   // overlaybd image config (config.v1.json)
    std::string service_config_path; // global service config, empty = default
    // per-device log file; empty = shared log from the global service config.
    // With one process per device, daemons sharing one log file interleave
    // indistinguishable lines and race on rotation, so a dedicated path per
    // device is recommended when running multiple devices.
    std::string log_path;
    // dedicated cache directory for this device (registry/gzip caches land in
    // subdirs); empty = shared dirs from the global service config. The file
    // cache's eviction/refill locking is in-process only, so daemons sharing
    // one cache directory can race -- REQUIRED when running multiple devices.
    std::string cache_dir;
    int dev_id = -1;                 // -1: let the kernel allocate
    int queue_depth = 128;
};

// Check /dev/ublk-control and report a clear error if unusable.
// Returns 0 on success, -1 (with message on stderr) otherwise.
int ublk_check_control_dev();

// Run one overlaybd-ublk device until SIGTERM/SIGINT (process == device).
// On readiness the block device path ("/dev/ublkbN\n") is
// written to ready_fd (the add command's sync-ready contract); ready_fd
// is closed afterwards unless it is stdout/stderr. Returns the process
// exit code.
int run_ublk_device(const UblkDeviceOpts &opts, int ready_fd);
