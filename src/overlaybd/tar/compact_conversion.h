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

#include <functional>
#include <photon/fs/filesystem.h>

namespace CompactConversion {

// image is the virtual ext4 I/O view; base is its writable LSMT view.
// All arguments are borrowed. A null base uses image. Only Append layers are supported.
// Scratch files, alignment and the ext4 lifetime are owned by this module.
// Return 0 on success, or -1 with errno on failure.
int mkfs(photon::fs::IFile *image, photon::fs::IFile *base = nullptr);

// Stage source once, then replay its metadata and install the payload mappings.
// finalize_source runs after staging and before opening or formatting ext4; it may
// finish input validation and must return 0 on success, or -1 with errno on failure.
// Failed preparation may leave unreferenced payload at EOF. Payload mappings start
// only after validation and metadata commit; a later mapping failure is not rolled back.
int apply(photon::fs::IFile *source, photon::fs::IFile *image, photon::fs::IFile *base,
          bool mkfs, const std::function<int()> &finalize_source = {});

} // namespace CompactConversion
