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

// Block-device hygiene for recycling pooled devices.
//
// A pooled device stays LIVE across tenants (that is the whole point: STOP and
// START are the ~8ms we are avoiding), so the kernel never drops its page
// cache on its own. Without the calls below a new tenant could read pages
// cached by the previous one. A server whose consumers only ever use O_DIRECT
// would not need this; ours may mount filesystems, so we must handle it.

// Whether /dev/ublkb<dev_id> currently backs a mount (scans
// /proc/self/mountinfo for the device's major:minor). A still-mounted device
// must never be recycled: BLKFLSBUF cannot invalidate pages a live filesystem
// holds. Returns true also on inspection failure -- fail safe.
bool ublk_device_is_mounted(int dev_id);

// Flush and invalidate the block device's page cache
// (ioctl BLKFLSBUF, same as `blockdev --flushbufs`). Returns 0 on success,
// -errno otherwise.
int ublk_device_flush_buffers(int dev_id);
