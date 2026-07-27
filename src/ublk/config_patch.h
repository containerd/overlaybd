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

// Rewrite every cache directory in a service config JSON to live under
// cache_dir (registry cache -> <cache_dir>/registry_cache, gzip cache ->
// <cache_dir>/gzip_cache). Pure string -> string transformation, kept apart
// from ublk_device.cpp so it is unit-testable without photon/ImageService.
//
// Returns 0 on success with the serialized result in out_json, -1 if
// base_json is not a valid JSON object.
int ublk_patch_cache_dirs(const std::string &base_json, const std::string &cache_dir,
                          std::string &out_json);
