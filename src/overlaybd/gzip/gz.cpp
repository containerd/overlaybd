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

#include "gz.h"
#include <fcntl.h>
#include <zlib.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/subfs.h>
#include <photon/fs/localfs.h>
#include <photon/net/socket.h>
#include "../../tools/sha256file.h"
#include "../gzindex/gzfile_index.h"
#include "photon/common/alog.h"
#include "photon/fs/localfs.h"
class GzAdaptorFile : public photon::fs::VirtualReadOnlyFile {
public:
    GzAdaptorFile() {
    }
    GzAdaptorFile(gzFile gzf) : m_gzf(gzf) {
    }
    ~GzAdaptorFile() {
        gzclose(m_gzf);
    }
    virtual photon::fs::IFileSystem *filesystem() override {
        return nullptr;
    }
    off_t lseek(off_t offset, int whence) override {
        return gzseek(m_gzf, offset, whence);
    }
    ssize_t read(void *buf, size_t count) override {
        return gzread(m_gzf, buf, count);
    }
    int fstat(struct stat *buf) override {
        return 0;
    }

    int load_data() {
        auto rc = gzread(m_gzf, m_buf, 1024 * 1024);
        if (rc < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to gzread");
        }
        m_cur = 0;
        m_left = rc;
        LOG_INFO(VALUE(rc));
        return rc;
    }
    // private:
    gzFile m_gzf;
    char m_buf[1024 * 1024];
    int m_cur = 0, m_left = 0;
};

class GzStreamFile :  public IGzFile {
public:
    GzStreamFile(IStream *sock, ssize_t st_size, bool index_save,
         const char* uid, const char *_workdir)
        : st_size(st_size), fstream(sock), workdir(_workdir){

        if (uid == nullptr) {
            timeval now;
            gettimeofday(&now, NULL);
            char suffix[32]{};
            sprintf(suffix, ".%lu", now.tv_sec*1000000 + now.tv_usec);
            fn_idx = fn_idx + suffix;
            fn_buff = fn_buff + suffix;
        } else {
            fn_idx = fn_idx + uid;
            fn_buff = fn_buff + uid;
        }

        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        // ret = inflateInit(&strm);
        inflateInit2(&strm, 47);
        ttin = ttout = strm.avail_out = 0;
        init_index_header(this, m_idx_header, GZ_CHUNK_SIZE, GZ_DICT_COMPERSS_ALGO,
                          GZ_COMPRESS_LEVEL);
        m_indexes.clear();
        m_lfs = photon::fs::new_localfs_adaptor(workdir.c_str());
        LOG_INFO("create buffer file(`) and indexfile(`)", fn_buff, fn_idx);
        if (index_save) {
            m_idx_file = m_lfs->open(fn_idx.c_str(), O_TRUNC | O_CREAT | O_RDWR, 0644);
            m_idx_filter = new_index_filter(&m_idx_header, &m_indexes, m_idx_file);
            fstream = new_sha256_file((IFile*)fstream, false);
        }

        buffer_file = photon::fs::open_localfile_adaptor(fn_buff.c_str(), O_TRUNC | O_CREAT | O_RDWR, 0644);
        LOG_INFO("create a GzStreamFile. workdir: `", workdir);
    };

    ~GzStreamFile() {
        (void)inflateEnd(&strm);
        delete m_idx_file;
        delete buffer_file;
        delete_index_filter(m_idx_filter);
        delete fstream;
        for (auto it:m_indexes) {
            delete it;
        }
        m_lfs->unlink(fn_buff.c_str());
        delete m_lfs;
    }

    UNIMPLEMENTED_POINTER(photon::fs::IFileSystem* filesystem() override);

    virtual off_t lseek(off_t offset, int whence) override {
        if (whence == SEEK_END) {
            return st_size - offset;
        }
        if (whence == SEEK_CUR) {
            assert(offset >= 0);
            char buf[32768];
            while (offset > 0) {
                auto len = offset;
                if (len > (off_t)sizeof(buf)) {
                    len = sizeof(buf);
                }
                auto readn = this->read(buf, len);
                if (readn <= 0){
                    LOG_ERRNO_RETURN(EIO, -1, "read buffer error");
                }
                offset -= readn;
            }
            return cur_offset;
        }
        LOG_ERRNO_RETURN(ESPIPE, -1, "unimplemented in GzStreamFile");
    }
    virtual int fstat(struct stat *buf) override {
        buf->st_size = st_size;
        return 0;
    }

    virtual ssize_t read(void *buf, size_t count) override {
        size_t n = 0;
        LOG_DEBUG("count: `", count);
        while (count > 0) {
            if (bf_len) {
                auto delta = ((size_t)bf_len > count ? count : bf_len);
                count -= delta;
                auto readn = buffer_file->read(buf, delta);
                LOG_DEBUG("copy ` bytes to ` from buffer.", readn, VALUE(buf));
                assert(readn == (ssize_t)delta);
                buf = (char *)buf + delta;
                bf_len -= delta;
                n += delta;
                continue;
            }
            LOG_DEBUG("trucate buffer file.");
            buffer_file->ftruncate(0);
            buffer_file->lseek(0, SEEK_SET);
            strm.avail_in = fstream->read(in, CHUNK);
            if (strm.avail_in < 0) {
                LOG_ERRNO_RETURN(0, -1, "read buffer from uds failed");
            }
            if (strm.avail_in == 0)
                break;
            if (!check_type) {
                if (!((uint8_t)in[0] == 0x1f && (uint8_t)in[1] == 0x8b)){
                    LOG_ERRNO_RETURN(EIO, -1, "buffer is not gzip type");
                }
                check_type = true;
            }
            LOG_DEBUG("recv: `", strm.avail_in);
            st_size += strm.avail_in;
            strm.next_in = in;
            int ret = 0;
            bf_start = 0;
            bf_len = 0;
            do {
                if (strm.avail_out == 0) {
                    strm.avail_out = CHUNK;
                    strm.next_out = out;
                }
                ttin += strm.avail_in;
                auto prev = strm.avail_out;
                auto copied = out + CHUNK - prev;
                ttout += strm.avail_out;
                ret = inflate(&strm, Z_BLOCK);
                assert(ret != Z_STREAM_ERROR); /* state not clobbered */
                switch (ret) {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR; /* and fall through */
                case Z_DATA_ERROR:
                case Z_MEM_ERROR:
                    (void)inflateEnd(&strm);
                    LOG_ERRNO_RETURN(0, -1, "zlib error: `", zError(ret));
                }
                ttin -= strm.avail_in;
                ttout -= strm.avail_out;
                have = prev - strm.avail_out;
                auto delta = (have > count ? count : have);
                assert(n + delta <= 65536);
                memcpy((char *)buf, copied, delta);
                buf = (char *)buf + delta;
                n += delta;
                LOG_DEBUG("` bytes copied to `", delta, VALUE(buf));
                if (have > count) {
                    LOG_DEBUG("` bytes bufferd", have - delta);
                    buffer_file->write(copied + delta, have - delta);
                    bf_len += (have - delta);
                }
                if (ret == Z_STREAM_END) {
                    m_idx_header.uncompress_file_size = ttout;
                    // TODO Here generate crc32 for last uncompressed data block
                    //  break;
                } else if (create_index_entry(strm, m_idx_filter, ttin, ttout, out) != 0) {
                    LOG_ERRNO_RETURN(ret, -1, "Failed to add_index_entry");
                }
                count -= delta;
            } while (strm.avail_in != 0);
            buffer_file->lseek(0, SEEK_SET);
        }
        cur_offset += n;
        LOG_DEBUG("current offset: `", cur_offset);
        return n;
    }

    virtual std::string sha256_checksum() override {
        if (sha256sum.empty()) {
            sha256sum = ((SHA256File*)fstream)->sha256_checksum();
        }
        return sha256sum;
    }

    virtual std::string save_index() override {
        if (save_index_to_file(m_idx_header, m_indexes, m_idx_file, st_size) != 0){
            LOG_ERRNO_RETURN(0, "", "save index failed");
        }
        auto dst_file = this->sha256_checksum() + ".gz_idx";
        LOG_INFO("save index as: `", dst_file);
        m_lfs->rename(fn_idx.c_str(), dst_file.c_str());
        return std::string(workdir) + "/" + dst_file;
    }
    ssize_t st_size = 0;
    IFile *m_file = nullptr;
    photon::fs::IFileSystem *m_fs = nullptr;
    bool check_type = false;

    const static int CHUNK = 32768;
    IStream *fstream;
    z_stream strm;
    Byte in[CHUNK]{}, out[CHUNK]{}; // buffer[10485760];
    /* allocate inflate state */
    size_t have = 0;
    off_t ttin, ttout;
    off_t bf_start = 0, bf_len = 0;

    off_t cur_offset = 0;
    photon::fs::IFileSystem *m_lfs = nullptr;
    IFile *buffer_file = nullptr;
    IFile *m_idx_file = nullptr;
    // const char *FN_BUFF_PREFIX = "/tmp/decompbuffer";
    // const char *FN_IDX_PREFIX = "/tmp/gzidx";
    std::string workdir;
    std::string fn_buff = "decomp_buffer";
    std::string fn_idx = "gz_idx";
    std::string sha256sum = "";
    IndexFilterRecorder *m_idx_filter = nullptr;
    INDEX m_indexes;
    IndexFileHeader m_idx_header;
};

photon::fs::IFile *open_gzfile_adaptor(const char *path) {
    gzFile gzf = gzopen(path, "r");
    if (gzf == nullptr)
        LOG_ERRNO_RETURN(0, nullptr, "failed to open gzip file ", VALUE(path));
    return new GzAdaptorFile(gzf);
}

IGzFile *open_gzstream_file(IStream *sock, ssize_t st_size,
                            bool save_idx, const char *uid, const char *workdir) {
    char buffer[1024]{};
    if (workdir == nullptr) {
        getcwd(buffer, sizeof(buffer));
        workdir = (char*)buffer;
    }
    return new GzStreamFile(sock, st_size, true, uid, workdir);
}
