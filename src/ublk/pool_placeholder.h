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

#include <cstdint>
#include <string>

// Warm-pool placeholder images. A pooled ublk device must be backed by *some*
// image at creation time; the daemon therefore builds its own throwaway sparse
// LSMT upper and points the pooled device at it until a real image is
// hot-swapped in. Built in-process (a sparse RW layer plus a minimal image
// config), without shelling out to overlaybd-create.
//
// Creates <dir>/<size_gb>-<slot>.data, .index and .json if absent; the config
// path is returned in out_config. Each pooled device needs its OWN placeholder
// (hence `slot`): two ImageFiles opening the same LSMT sparse layer read-write
// would corrupt each other's layer state. Sparse files cost no real disk.
// Idempotent: an existing triple is reused as is. Returns 0 on success, -1
// with a logged reason otherwise.
int ublk_pool_placeholder(const std::string &dir, uint64_t size_gb, int slot,
                          std::string &out_config);
