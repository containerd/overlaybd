/*
  * virtual-file.cpp
  * 
  * Copyright (C) 2021 Alibaba Group.
  * 
  * This program is free software; you can redistribute it and/or
  * modify it under the terms of the GNU General Public License
  * as published by the Free Software Foundation; either version 2
  * of the License, or (at your option) any later version.
  * 
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  * 
  * See the file COPYING included with this distribution for more details.
*/
#include "virtual-file.h"
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include "../utility.h"
#include "../iovector.h"
#include "../alog.h"

namespace FileSystem
{
    ssize_t VirtualFile::read(void *buf, size_t count)
    {
        auto ret = pread(buf, count, m_offset);
        if (ret > 0) m_offset += ret;
        return ret;
    }
    ssize_t VirtualFile::readv(const struct iovec *iov, int iovcnt)
    {
        auto ret = preadv(iov, iovcnt, m_offset);
        if (ret > 0) m_offset += ret;
        return ret;
    }
    ssize_t VirtualFile::write(const void *buf, size_t count)
    {
        auto ret = pwrite(buf, count, m_offset);
        if (ret > 0) m_offset += ret;
        return ret;
    }
    ssize_t VirtualFile::writev(const struct iovec *iov, int iovcnt)
    {
        auto ret= pwritev(iov, iovcnt, m_offset);
        if (ret > 0) m_offset += ret;
        return ret;
    }
    
    ssize_t VirtualFile::pread(void *buf, size_t count, off_t offset)
    {
        iovec v{buf, count};
        return preadv(&v, 1, offset);
    }
    ssize_t VirtualFile::pwrite(const void *buf, size_t count, off_t offset)
    {
        iovec v{(void*)buf, count};
        return pwritev(&v, 1, offset);
    }
    ssize_t VirtualFile::preadv(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return piov(_and_pread(), iov, iovcnt, offset);
    }
    ssize_t VirtualFile::pwritev(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return piov(_and_pwrite(), iov, iovcnt, offset);
    }
    off_t VirtualFile::lseek(off_t offset, int whence)
    {
        if (whence == SEEK_SET) {
            m_offset = offset;
        } else if (whence == SEEK_CUR) {
            m_offset += offset;
        } else if (whence == SEEK_END) {
            struct ::stat stat;
            auto ret = fstat(&stat);
            if (ret < 0)
                return -1;
            m_offset = stat.st_size + offset;
        } else {
            return -1;
        }
        return m_offset;
    }
    ssize_t VirtualFile::piov_nocopy(FuncPIO f, const struct iovec *iov, int iovcnt, off_t offset)
    {
        ssize_t count = 0;
        for (auto v: iovector_view((iovec*)iov, iovcnt))
        {
            ssize_t ret = (this->*f)(v.iov_base, v.iov_len, offset);
            if (ret < (ssize_t)v.iov_len)
            {
                LOG_ERROR("failed to ", is_readf(f) ? "read" : "write");
                return -1;
            }
            offset += v.iov_len;
            count  += v.iov_len;
        }
        return count;
    }
    ssize_t VirtualFile::piov_copy(FuncPIO f, const struct iovec *iov, int iovcnt, off_t offset)
    {
        if (iovcnt == 0) {
            return 0;
        } else if (iovcnt == 1) {
            return (this->*f)((void*)iov[0].iov_base, iov[0].iov_len, offset);
        }
        iovector_view va((iovec*)iov, iovcnt);
        size_t count = va.sum();
        
        auto ptr = new char[count + 4096];
        std::unique_ptr<char[]> deleter(ptr);
        auto buf = align_ptr(ptr, 4096);
        if (is_readf(f))
        {
            ssize_t ret = pread(buf, count, offset);
            return (ret <= 0) ? ret :
                va.memcpy_from(buf, ret);
        }
        else
        {
            size_t ret = va.memcpy_to(buf, count);
            assert(ret == count);
            _unused(ret);
            return pwrite(buf, count, offset) ;
        }
    }

#ifdef __linux__
    #ifndef FALLOC_FL_KEEP_SIZE
    #define FALLOC_FL_KEEP_SIZE     0x01 /* default is extend size */
    #endif
    #ifndef FALLOC_FL_PUNCH_HOLE
    #define FALLOC_FL_PUNCH_HOLE	0x02 /* de-allocates range */
    #endif
    int IFile::trim(off_t offset, off_t len)
    {
        int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
        return this->fallocate(mode, offset, len);
    }

    #ifndef FALLOC_FL_ZERO_RANGE
    #define FALLOC_FL_ZERO_RANGE            0x10
    #endif
    int IFile::zero_range(off_t offset, off_t len)
    {
        int mode = FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE;
        int ret = this->fallocate(mode, offset, len);
        if (ret == 0 || errno != EINVAL)
            return ret;

        // assert(failed with EINVAL)
        // try to trim() and re-allocate file space
        ret = this->trim(offset, len);
        if (ret < 0)
            return -1;
        
        return this->fallocate(FALLOC_FL_KEEP_SIZE, offset, len);
    }
#endif //__linux__
}
