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
#include <vector>

// Request/response codec of the overlaybd-ublkd control protocol
// (HTTP over UDS, /v1/* endpoints). Pure JSON logic on
// rapidjson, deliberately free of photon/ublksrv so it unit-tests like
// cli.cpp/config_patch.cpp. Unknown JSON fields are ignored (forward
// compatibility).

struct UblkdAddRequest {
    std::string config;      // image config path, required
    std::string instance_id; // reserved (unused)
    int dev_id = -1;         // -1: kernel allocates
    int queue_depth = 128;
};

struct UblkdDeviceInfo {
    int dev_id = -1;
    std::string dev_path; // /dev/ublkbN
    std::string config;
    bool writable = false;
    std::string state; // "running" / "stopping"
    std::string mode;  // "exclusive" / "shared"
    int refcount = 0;  // shared devices only (0 for exclusive)
};

// parse POST /v1/add body; 0 = ok, -1 = malformed (err says why)
int ublkd_parse_add(const std::string &body, UblkdAddRequest &req, std::string &err);

// parse POST /v1/del body ({"dev_id":N}); 0 = ok, -1 = malformed
int ublkd_parse_del(const std::string &body, int &dev_id, std::string &err);

struct UblkdAcquireRequest {
    std::string config;              // image config path, required
    std::string mode = "shared";     // "shared" (default) | "exclusive"
};

// parse POST /v1/acquire body; 0 = ok, -1 = malformed. mode defaults to
// "shared"; any value other than shared/exclusive is rejected.
int ublkd_parse_acquire(const std::string &body, UblkdAcquireRequest &req,
                        std::string &err);

struct UblkdResizeRequest {
    int dev_id = -1;
    uint64_t size_gb = 0;   // new virtual size, must grow
    bool resize_fs = false; // also grow the ext4 inside
};

// parse POST /v1/resize body; 0 = ok, -1 = malformed
int ublkd_parse_resize(const std::string &body, UblkdResizeRequest &req,
                       std::string &err);

// response builders (single-line JSON)
std::string ublkd_msg_ok();                                       // {"ok":true}
std::string ublkd_msg_error(const std::string &error);            // {"ok":false,...}
std::string ublkd_msg_added(int dev_id, const std::string &path); // add success
// acquire success: like added, plus mode and refcount
std::string ublkd_msg_acquired(int dev_id, const std::string &path,
                               const std::string &mode, int refcount);
// release success: {"ok":true,"refcount":n} (0 = device was torn down)
std::string ublkd_msg_released(int refcount);
std::string ublkd_msg_list(const std::vector<UblkdDeviceInfo> &devices);
std::string ublkd_msg_ping(const std::string &version);
// GET /v1/pool: warm pool state; low == 0 means pooling is disabled
std::string ublkd_msg_pool(int low, int high, int idle, uint64_t size_gb,
                           uint64_t hits, uint64_t misses);
