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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <list>
#include <sys/stat.h>
#include <algorithm>
#include "gzfile_index.h"
#include "photon/common/alog.h"
#include "photon/common/alog-stdstring.h"
#include "photon/fs/localfs.h"
#include "photon/photon.h"
#include "photon/fs/virtual-file.h"
#include "photon/common/checksum/crc32c.h"
#include "photon/thread/thread.h"

namespace FileSystem {
using namespace photon::fs;

class GzFile : public VirtualReadOnlyFile {
public:
    bool m_file_ownership = false;
    GzFile() = delete;
    explicit GzFile(photon::fs::IFile* gzip_file, photon::fs::IFile* index);
    virtual ~GzFile(){
        if (m_file_ownership) {
            delete gzip_file_;
            delete index_file_;
        }
    };

public:
    virtual ssize_t pread(void *buf, size_t count, off_t offset) override;
    virtual int fstat(struct stat *buf) override;
    virtual IFileSystem* filesystem() { return NULL; };
    virtual int close() override {return 0;};

    UNIMPLEMENTED(ssize_t write(const void *buf, size_t count));
    UNIMPLEMENTED(ssize_t writev(const struct iovec *iov, int iovcnt));
    UNIMPLEMENTED(ssize_t read(void *buf, size_t count));
    UNIMPLEMENTED(ssize_t readv(const struct iovec *iov, int iovcnt));
    UNIMPLEMENTED(off_t lseek(off_t offset, int whence));

private:
    IFile *gzip_file_ = nullptr;
    IFile *index_file_ = nullptr;
    struct IndexFileHeader index_header_;
    INDEX index_;
    bool inited_ = false;
    photon::mutex init_mutex_;
    int init();
    int parse_index();
    IndexEntry *seek_index(INDEX &index, off_t offset);
    ssize_t extract(const struct IndexEntry *found_idx,
                    off_t offset, unsigned char *buf, int len);
    int get_dict_by_index(const IndexEntry *found_idx, unsigned char *window_buf);
};

static int zlib_decompress(unsigned char *in, int in_len, unsigned char *out, int& out_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int ret = inflateInit(&strm);
    if (ret != Z_OK) {
        LOG_ERRNO_RETURN(0, -1, "Failed to inflateInit");
    }
    DEFER(inflateEnd(&strm));

    strm.avail_in = in_len;
    strm.next_in = in;
    strm.avail_out = out_len;
    strm.next_out = out;
    ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        LOG_ERRNO_RETURN(0, -1, "ret != Z_STREAM_END, ret:`", ret);
    }
    out_len = out_len - strm.avail_out;
    return 0;
}

GzFile::GzFile(photon::fs::IFile* gzip_file, photon::fs::IFile* index) {
    gzip_file_ = gzip_file;
    index_file_ = index;
}
int GzFile::fstat(struct stat *buf) {
    if (!inited_) {
        if (init() != 0 ) {
            LOG_ERRNO_RETURN(0, -1, "Fail init()");
        }
    }
    int ret = gzip_file_->fstat(buf);
    buf->st_size = index_header_.uncompress_file_size;
    return ret;
}
int GzFile::parse_index() {
    int index_buf_len = index_header_.index_num * index_header_.index_size;
    unsigned char *index_buf = new unsigned char[index_buf_len];
    DEFER(delete []index_buf);

    int idx_area_buf_len = index_header_.index_area_len;
    unsigned char *idx_area_buf = new unsigned char[idx_area_buf_len];
    DEFER(delete []idx_area_buf);

    if (index_file_->pread(idx_area_buf, idx_area_buf_len, index_header_.index_start) != idx_area_buf_len) {
        LOG_ERRNO_RETURN(0, -1, "Failed to index_file_->pread, idx_area_buf_len:`, offset:`", idx_area_buf_len, index_header_.index_start + 0);
    }

    if (index_header_.dict_compress_algo != 0) {
        if (zlib_decompress(idx_area_buf, idx_area_buf_len, index_buf, index_buf_len) != 0) {
            LOG_ERRNO_RETURN(0, -1, "Faild to zlib_decompress, idx_area_buf_len:`, index_buf_len:`", idx_area_buf_len, index_buf_len);
        }
        if (index_buf_len != index_header_.index_num * index_header_.index_size) {
            LOG_ERRNO_RETURN(0, -1, "Wrong uncompressed buffer len for index.");
        }
    } else {
        if (index_header_.index_area_len != index_buf_len) {
            LOG_ERRNO_RETURN(0, -1, "Wrong index area len when dict_compress_algo == 0");
        }
        memcpy(index_buf, idx_area_buf, index_buf_len);
    }

    for (int64_t i=0; i<index_header_.index_num ; i++) {
        struct IndexEntry *p= new (struct IndexEntry);
        off_t offset = i * sizeof(IndexEntry);
        memcpy(p, index_buf + offset, sizeof(IndexEntry));
        index_.push_back(p);
    }
    return 0;
}

int GzFile::init() {
    SCOPED_LOCK(init_mutex_);
    if (inited_) {
        return 0;
    }

    struct stat sbuf;
    if (index_file_->fstat(&sbuf) != 0 ) {
        LOG_ERRNO_RETURN(0, -1, "Failed to index_file_->fstat");
    }
    int64_t idx_file_size = sbuf.st_size;
    LOG_INFO("idx_file_size:`, sizeof(index_header_):`", idx_file_size, sizeof(index_header_));

    if (static_cast<uint64_t>(idx_file_size) < sizeof(index_header_)) {
        LOG_ERRNO_RETURN(0, -1, "size of index file is too small. idx_file_size:`", idx_file_size);
    }

    if (index_file_->pread(&index_header_, sizeof(index_header_), 0) != sizeof(index_header_)) {
        LOG_ERRNO_RETURN(0, -1, "Failed to index_file_->pread");
    }
    if (idx_file_size != index_header_.index_file_size) {
        LOG_ERRNO_RETURN(0, -1, "Wrong index file size. idx_file_size:`!=`", idx_file_size, index_header_.index_file_size+0);
    }

    LOG_INFO("`", index_header_.to_str());

    if (index_header_.cal_crc() != index_header_.crc) {
        LOG_ERRNO_RETURN(0, -1, "Faild to check CRC of index_header");
    }
    if (index_header_.major_version != 1) {
        LOG_ERRNO_RETURN(0, -1, "Wrong index version, required:1, value:`", index_header_.major_version + 0);
    }

    if (sizeof(IndexEntry) != index_header_.index_size) {
        LOG_ERRNO_RETURN(0, -1, "Failed check index_header_.index_size. ` != `", sizeof(IndexEntry), index_header_.index_size + 0);
    }
    if (strncmp(GZFILE_INDEX_MAGIC, index_header_.magic, strlen(GZFILE_INDEX_MAGIC)) != 0) {
        LOG_ERRNO_RETURN(0, -1, "Wrong magic ` != `", GZFILE_INDEX_MAGIC, index_header_.magic);
    }

    if (static_cast<int64_t>(idx_file_size) != index_header_.index_start + index_header_.index_area_len) {
        LOG_INFO("Wrong idx_file_size:`", idx_file_size);
        LOG_ERRNO_RETURN(0, -1, "size of index file is wrong. idx_file_size:`,", idx_file_size);
    }

    struct stat stat_buf;
    if (gzip_file_->fstat(&stat_buf) != 0) {
        LOG_ERRNO_RETURN(0, -1, "Wrong magic ` != `", GZFILE_INDEX_MAGIC, index_header_.magic);
    }
    if (index_header_.gzip_file_size != stat_buf.st_size) {
        LOG_ERRNO_RETURN(0, -1, "Failed check size of gzfile. ` != `", index_header_.gzip_file_size + 0, stat_buf.st_size + 0);
    }

    if (parse_index() != 0) {
        LOG_ERRNO_RETURN(0, -1, "Failed parse_index()");
    }

    inited_ = true;
    return 0;
}

static bool indx_compare(struct IndexEntry* i, struct IndexEntry* j) { return i->de_pos < j->de_pos; }
IndexEntry *GzFile::seek_index(INDEX &index, off_t offset) {
    if (index.size() == 0) {
        return nullptr;
    }
    struct IndexEntry tmp;
    tmp.de_pos = offset;
    INDEX::iterator iter = std::upper_bound(index.begin(), index.end(), &tmp, indx_compare);
    if (iter == index.end()) {
        return index.at(index.size() - 1);
    }
    int idx = iter - index.begin();
    if (idx > 0) {
        idx --;
    }
    return index.at(idx);
}

int GzFile::get_dict_by_index(const IndexEntry *found_idx, unsigned char *dict_buf) {
    if (index_header_.dict_compress_algo == 0) {
        if (found_idx->win_len != WINSIZE) {
            LOG_ERRNO_RETURN(0, -1, "Wrong window size:`", found_idx->win_len+0);
        }
        if (index_file_->pread(dict_buf, found_idx->win_len, found_idx->win_pos) != found_idx->win_len) {
            LOG_ERRNO_RETURN(0, -1, "Fail to index_file_->pread, offset:`, len:`", found_idx->win_pos+0, found_idx->win_len+0);
        }
        return 0;
    }

    unsigned char *tmp_buf = new unsigned char[found_idx->win_len];
    DEFER(delete []tmp_buf);
    if (index_header_.dict_compress_algo != 1) {
        LOG_ERRNO_RETURN(0, -1, "Wrong dict compress algorithm:`", index_header_.dict_compress_algo);
    }

    if (index_file_->pread(tmp_buf, found_idx->win_len, found_idx->win_pos) != found_idx->win_len) {
        LOG_ERRNO_RETURN(0, -1, "Fail to index_file_->pread, offset:`, len:`", found_idx->win_pos+0, found_idx->win_len+0);
    }

    int window_buf_len = WINSIZE;
    if (zlib_decompress(tmp_buf, found_idx->win_len, dict_buf, window_buf_len) != 0) {
        LOG_ERRNO_RETURN(0, -1, "Failed to dict_zlib_decompress.");
    }

    if (window_buf_len != WINSIZE) {
        LOG_ERRNO_RETURN(0, -1, "window_buf_len != `", WINSIZE);
    }

    return 0;
}

ssize_t GzFile::extract(
        const struct IndexEntry *found_idx,
        off_t offset,
        unsigned char *buf, int buf_len) {
#define CHUNK 65536
    unsigned char inbuf[CHUNK];
    unsigned char dict[WINSIZE];
    unsigned char discard[CHUNK];

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    int ret = inflateInit2(&strm, -15);
    if (ret != Z_OK) {
        LOG_ERRNO_RETURN(0, -1, "Fail to inflateInit2(&strm, -15)");
    }
    DEFER(inflateEnd(&strm));

    off_t start_pos = found_idx->en_pos - (found_idx->bits ? 1 : 0);
    // ret = gzip_file_->lseek(start_pos, SEEK_SET);
    if (ret == -1){
        LOG_ERRNO_RETURN(0, -1, "Fail to gzip_file->lseek(`, SEEK_SET)", start_pos);
    }
    if (found_idx->bits) {
        unsigned char tmp;
        if (gzip_file_->pread(&tmp, 1, start_pos) != 1) {
            LOG_ERRNO_RETURN(0, -1, "Fail to gzip_file->pread");
        }
        start_pos++;
        inflatePrime(&strm, found_idx->bits, tmp >> (8 - found_idx->bits));
    }

    if (get_dict_by_index(found_idx, dict) != 0) {
        LOG_ERRNO_RETURN(0, -1, "Faild to get window data.");
    }
    inflateSetDictionary(&strm, dict, WINSIZE);

    offset -= found_idx->de_pos;
    strm.avail_in = 0;
    bool skip = true;
    do {
        if (offset == 0 && skip) {
            strm.avail_out = buf_len;
            strm.next_out = buf;
            skip = false;
        }
        if (offset > CHUNK) {
            strm.avail_out = CHUNK;
            strm.next_out = discard;
            offset -= CHUNK;
        } else if (offset != 0) {
            strm.avail_out = static_cast<unsigned int>(offset);
            strm.next_out = discard;
            offset = 0;
        }

        do {
            if (strm.avail_in == 0) {
                ssize_t read_cnt = gzip_file_->pread(inbuf, CHUNK, start_pos);
                if (read_cnt < 0 ) {
                    LOG_ERRNO_RETURN(0, -1, "Fail to gzip_file->pread(input, CHUNK, `)", start_pos);
                }
                if (read_cnt == 0) {
                    LOG_ERRNO_RETURN(Z_DATA_ERROR, -1, "Fail to gzip_file->pread(input, CHUNK, `)", start_pos);
                }
                start_pos += read_cnt;
                strm.avail_in = read_cnt;
                strm.next_in = inbuf;
            }
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_END) {
                break;
            }
            if (ret != Z_OK && ret != Z_BUF_ERROR) {
                LOG_ERRNO_RETURN(0, -1, "Fail to inflate. ret:`", ret);
            }
        } while (strm.avail_out != 0);
        if (ret == Z_STREAM_END) {
            break;
        }
    } while (skip);
    if (skip) {
        return 0;
    }
    //LOG_DEBUG("offset:`,len:`,return:`", offset, len, len - strm.avail_out);
    return buf_len - strm.avail_out;
#undef CHUNK
}

ssize_t GzFile::pread(void *buf, size_t count, off_t offset) {
    if (!inited_) {
        if (init() != 0 ) {
            LOG_ERRNO_RETURN(0, -1, "Fail init()", offset);
        }
    }
    if (offset < 0) {
        LOG_ERRNO_RETURN(EINVAL, -1, "invalid offset: ` < 0", offset);
    }

    struct IndexEntry * p = seek_index(index_, offset);
    if (p == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "Failed to seek_index(,`)", offset);
    }

    return extract(p, offset, (unsigned char*)buf, count);
}

} // namespace FileSystem

photon::fs::IFile* new_gzfile(photon::fs::IFile* gzip_file, photon::fs::IFile* index, bool ownership) {
    if (!gzip_file || !index) {
        LOG_ERRNO_RETURN(0, nullptr, "invalid file ptr. file: `, `", gzip_file, index);
    }
    auto rst = new FileSystem::GzFile(gzip_file, index);
    rst->m_file_ownership = ownership;
    return rst;
}

bool is_gzfile(photon::fs::IFile* file) {
    char buf[4] = {0};
    file->read(buf, 2);
    file->lseek(0, 0);
    return (uint8_t)buf[0] == 0x1f && (uint8_t)buf[1] == 0x8b;
}
