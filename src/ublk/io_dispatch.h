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
#include <sys/types.h>
#include <sys/uio.h>

// IO dispatch layer decoupled from the ublksrv event loop:
// keeps the mapping unit-testable without a ublk-capable kernel, and
// replaceable if the event-loop integration ever has to fall back to the
// official demo_event (two-thread) model.
//
// UblkIOTarget is the minimal seam over ImageFile; unit tests substitute
// an in-memory implementation.
struct UblkIOTarget {
    virtual ~UblkIOTarget() = default;
    virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) = 0;
    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) = 0;
    virtual int fdatasync() = 0;
    virtual int fallocate(int mode, off_t offset, off_t len) = 0;
};

// Map one ublk io descriptor (op + byte range) to an UblkIOTarget call.
// Follows ublk completion semantics: returns transferred bytes on success
// (0 for FLUSH/DISCARD/WRITE_ZEROES), negative errno on failure.
int ublk_dispatch_io(UblkIOTarget *tgt, uint8_t op, uint64_t offset_bytes,
                     uint32_t len_bytes, void *buf);
