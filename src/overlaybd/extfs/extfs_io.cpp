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

#include <sys/stat.h>

#include <photon/common/alog.h>
#include <photon/common/callback.h>
#include <photon/thread/thread.h>

#include "extfs.h"

errcode_t extfs_open(const char *name, int flags, io_channel *channel);
errcode_t extfs_close(io_channel channel);
errcode_t extfs_set_blksize(io_channel channel, int blksize);
errcode_t extfs_read_blk(io_channel channel, unsigned long block, int count, void *buf);
errcode_t extfs_read_blk64(io_channel channel, unsigned long long block, int count, void *buf);
errcode_t extfs_write_blk(io_channel channel, unsigned long block, int count, const void *buf);
errcode_t extfs_write_blk64(io_channel channel, unsigned long long block, int count, const void *buf);
errcode_t extfs_flush(io_channel channel);
errcode_t extfs_discard(io_channel channel, unsigned long long block, unsigned long long count);
errcode_t extfs_cache_readahead(io_channel channel, unsigned long long block, unsigned long long count);
errcode_t extfs_zeroout(io_channel channel, unsigned long long block, unsigned long long count);

struct unix_private_data {
    int magic;
    int dev;
    int flags;
    int align;
    int access_time;
    ext2_loff_t offset;
    void *bounce;
    struct struct_io_stats io_stats;
};

template <typename T>
struct CallBack;

template <typename Ret, typename... Params>
struct CallBack<Ret(Params...)> {
    template <typename... Args>
    static Ret callback(Args... args) { return cb.fire(args...); }
    static Delegate<Ret, Params...> cb;
};

// Initialize the static member.
template <typename Ret, typename... Params>
Delegate<Ret, Params...> CallBack<Ret(Params...)>::cb;

// add for debug
static uint64_t total_read_cnt = 0;
static uint64_t total_write_cnt = 0;

class ExtfsIOManager : public IOManager {
public:
    using OpenFunc = errcode_t(const char *, int, io_channel *);
    using OpenCallback = CallBack<OpenFunc>;

    ExtfsIOManager(photon::fs::IFile *file) : m_file(file) {
        OpenCallback::cb.bind(this, &ExtfsIOManager::m_extfs_open);
        auto extfs_open = static_cast<OpenFunc *>(OpenCallback::callback);

        extfs_io_manager = {
            .magic              = EXT2_ET_MAGIC_IO_MANAGER,
            .name               = "extfs I/O Manager",
            .open               = extfs_open,
            .close              = extfs_close,
            .set_blksize        = extfs_set_blksize,
            .read_blk           = extfs_read_blk,
            .write_blk          = extfs_write_blk,
            .flush              = extfs_flush,
            .write_byte         = nullptr,
            .set_option         = nullptr,
            .get_stats          = nullptr,
            .read_blk64         = extfs_read_blk64,
            .write_blk64        = extfs_write_blk64,
            .discard            = extfs_discard,
            .cache_readahead    = extfs_cache_readahead,
            .zeroout            = extfs_zeroout,
            .reserved           = {},
        };
    }

    ~ExtfsIOManager() {
        LOG_INFO(VALUE(total_read_cnt), VALUE(total_write_cnt));
    }

    virtual io_manager get_io_manager() override {
        return &extfs_io_manager;
    }

private:
    errcode_t m_extfs_open(const char *name, int flags, io_channel *channel) {
        LOG_INFO(VALUE(name));
        DEFER(LOG_INFO("opened"));
        ExtfsIOManager::mutex.lock();
        DEFER(ExtfsIOManager::mutex.unlock());
        io_channel io = nullptr;
        struct unix_private_data *data = nullptr;
        errcode_t retval;
        ext2fs_struct_stat st;

        retval = ext2fs_get_mem(sizeof(struct struct_io_channel), &io);
        if (retval)
            return -retval;
        memset(io, 0, sizeof(struct struct_io_channel));
        io->magic = EXT2_ET_MAGIC_IO_CHANNEL;
        retval = ext2fs_get_mem(sizeof(struct unix_private_data), &data);
        if (retval)
            return -retval;

        io->manager = &extfs_io_manager;
        retval = ext2fs_get_mem(strlen(name) + 1, &io->name);
        if (retval)
            return -retval;

        strcpy(io->name, name);
        io->private_data = data;
        io->block_size = 1024;
        io->read_error = 0;
        io->write_error = 0;
        io->refcount = 1;
        io->flags = 0;
        io->reserved[0] = reinterpret_cast<std::uintptr_t>(m_file);
        LOG_DEBUG(VALUE(m_file), VALUE((void *)io->reserved[0]));

        memset(data, 0, sizeof(struct unix_private_data));
        data->magic = EXT2_ET_MAGIC_UNIX_IO_CHANNEL;
        data->io_stats.num_fields = 2;
        data->flags = flags;
        data->dev = 0;

        *channel = io;
        return 0;
    }

    struct struct_io_manager extfs_io_manager;
    photon::fs::IFile *m_file;
    static photon::mutex mutex;
};

// Initialize the static member.
photon::mutex ExtfsIOManager::mutex;

IOManager *new_io_manager(photon::fs::IFile *file) {
    return new ExtfsIOManager(file);
}

errcode_t extfs_close(io_channel channel) {
    LOG_INFO("extfs close");
    return ext2fs_free_mem(&channel);
}

errcode_t extfs_set_blksize(io_channel channel, int blksize) {
    LOG_DEBUG(VALUE(blksize));
    channel->block_size = blksize;
    return 0;
}

errcode_t extfs_read_blk(io_channel channel, unsigned long block, int count, void *buf) {
    off_t offset = (ext2_loff_t)block * channel->block_size;
    ssize_t size = count < 0 ? -count : (ext2_loff_t)count * channel->block_size;
    auto file = reinterpret_cast<photon::fs::IFile *>(channel->reserved[0]);
    // LOG_DEBUG("read ", VALUE(offset), VALUE(size));
    auto res = file->pread(buf, size, offset);
    if (res == size) {
        total_read_cnt += size;
        return 0;
    }
    LOG_ERROR("failed to pread, got `, expect `", res, size);
    return -1;
}

errcode_t extfs_read_blk64(io_channel channel, unsigned long long block, int count, void *buf) {
    return extfs_read_blk(channel, block, count, buf);
}

errcode_t extfs_write_blk(io_channel channel, unsigned long block, int count, const void *buf) {
    off_t offset = (ext2_loff_t)block * channel->block_size;
    ssize_t size = count < 0 ? -count : (ext2_loff_t)count * channel->block_size;
    auto file = reinterpret_cast<photon::fs::IFile *>(channel->reserved[0]);
    // LOG_DEBUG("write ", VALUE(offset), VALUE(size));
    auto res = file->pwrite(buf, size, offset);
    if (res == size) {
        total_write_cnt += size;
        return 0;
    }
    LOG_ERROR("failed to pwrite, got `, expect `", res, size);
    return -1;
}

errcode_t extfs_write_blk64(io_channel channel, unsigned long long block, int count, const void *buf) {
    return extfs_write_blk(channel, block, count, buf);
}

errcode_t extfs_flush(io_channel channel) {
    return 0;
}

errcode_t extfs_discard(io_channel channel, unsigned long long block, unsigned long long count) {
    return 0;
}

errcode_t extfs_cache_readahead(io_channel channel, unsigned long long block, unsigned long long count) {
    return 0;
}

errcode_t extfs_zeroout(io_channel channel, unsigned long long block, unsigned long long count) {
    return 0;
}
