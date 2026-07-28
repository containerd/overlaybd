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
// (HTTP over UDS, /v1/* endpoints -- ADR-0006). Pure JSON logic on
// rapidjson, deliberately free of photon/ublksrv so it unit-tests like
// cli.cpp/config_patch.cpp. Unknown JSON fields are ignored (forward
// compatibility).

struct UblkdAddRequest {
    std::string config;      // image config path, required
    std::string instance_id; // reserved (unused until M2 cache work)
    int dev_id = -1;         // -1: kernel allocates
    int queue_depth = 128;
};

struct UblkdDeviceInfo {
    int dev_id = -1;
    std::string dev_path; // /dev/ublkbN
    std::string config;
    bool writable = false;
    std::string state; // "running" / "stopping"
};

// parse POST /v1/add body; 0 = ok, -1 = malformed (err says why)
int ublkd_parse_add(const std::string &body, UblkdAddRequest &req, std::string &err);

// parse POST /v1/del body ({"dev_id":N}); 0 = ok, -1 = malformed
int ublkd_parse_del(const std::string &body, int &dev_id, std::string &err);

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
std::string ublkd_msg_list(const std::vector<UblkdDeviceInfo> &devices);
std::string ublkd_msg_ping(const std::string &version);
