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
#include "io_dispatch.h"

#include <cerrno>

#include <ublk_cmd.h>

int ublk_dispatch_io(UblkIOTarget *tgt, uint8_t op, uint64_t offset_bytes,
                     uint32_t len_bytes, void *buf) {
    switch (op) {
    case UBLK_IO_OP_READ: {
        struct iovec iov {
            buf, len_bytes
        };
        ssize_t ret = tgt->preadv(&iov, 1, offset_bytes);
        // partial read is an error, same as the TCMU frontend
        return (ret == (ssize_t)len_bytes) ? (int)ret : -EIO;
    }

    case UBLK_IO_OP_WRITE: {
        struct iovec iov {
            buf, len_bytes
        };
        ssize_t ret = tgt->pwritev(&iov, 1, offset_bytes);
        if (ret == (ssize_t)len_bytes)
            return (int)ret;
        // keep the TCMU frontend's EROFS special case (write to sealed image)
        return (errno == EROFS) ? -EROFS : -EIO;
    }

    case UBLK_IO_OP_FLUSH:
        return (tgt->fdatasync() == 0) ? 0 : -EIO;

    case UBLK_IO_OP_DISCARD:
    case UBLK_IO_OP_WRITE_ZEROES:
        // same as the TCMU WRITE_SAME+UNMAP path:
        // fallocate(3) = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE
        return (tgt->fallocate(3, offset_bytes, len_bytes) == 0) ? 0 : -EIO;

    default:
        return -ENOTSUP;
    }
}
