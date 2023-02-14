#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <sys/fcntl.h>
#include "gzfile_index.h"
#include "photon/common/alog.h"
#include "photon/common/alog-stdstring.h"
#include "photon/fs/localfs.h"
#include "photon/photon.h"

#define DICT_COMPRESS_ALGO_NONE 0
#define DICT_COMPRESS_ALGO_ZLIB 1

#define EACH_DEFLATE_BLOCK_BIT  0X080
#define LAST_DEFLATE_BLOCK_BIT   0X40

static int zlib_compress(int level, unsigned char *in, int in_len, unsigned char *out, int& out_len) {
	z_stream strm;
	memset(&strm, 0, sizeof(strm));

	int ret = deflateInit(&strm, level);
	if (ret != Z_OK) {
		LOG_ERRNO_RETURN(0, -1, "Failed to deflateInit. level:`", level);
	}
	DEFER(deflateEnd(&strm));

	strm.next_in = in;
	strm.avail_in = in_len;

	strm.avail_out = out_len;
    strm.next_out = out;

    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        LOG_ERRNO_RETURN(0, -1, "ret != Z_STREAM_END, ret:`", ret);
    }

    out_len = out_len - strm.avail_out;
    return 0;
}

static int dict_compress(const IndexFileHeader& h,
		unsigned char *dict, int dict_len, unsigned char *&out, int& out_len) {
	if (h.dict_compress_algo == 0) {
	    out = dict;
	    out_len = dict_len;
	    return 0;
	}

	if (h.dict_compress_algo == 1) {
	    if (zlib_compress(h.dict_compress_level, dict, WINSIZE, out , out_len) != 0) {
            LOG_ERRNO_RETURN(0, -1, "Failed to dict_compress");
	    }
	    return 0;
	}
	LOG_ERRNO_RETURN(0, -1, "Wrong compress algorithm. h.dict_compress_algo:`", h.dict_compress_algo);
	return -1;
}

static int add_index_entry(unsigned char *temp_buf, int32_t temp_buf_len, IndexFileHeader& h, int bits, off_t en_pos, off_t de_pos,
		unsigned int left, unsigned char *window, INDEX &index, photon::fs::IFile* index_file) {
	struct IndexEntry* p = new IndexEntry;
    p->bits = bits;
    p->en_pos = en_pos;
    p->de_pos = de_pos;
    p->win_pos= h.index_start;
    unsigned char dict[WINSIZE];
    if (left) {
        memcpy(dict, window + WINSIZE - left, left);
    }
    if (left < WINSIZE) {
        memcpy(dict + left, window, WINSIZE - left);
    }

    if (dict_compress(h, dict, WINSIZE, temp_buf, temp_buf_len) != 0) {
        LOG_ERRNO_RETURN(0, -1, "Failed to dict_compress");
    }

    if (index_file->pwrite(temp_buf, temp_buf_len, p->win_pos) != temp_buf_len) {
        LOG_ERRNO_RETURN(0, -1, "Failed to index_file->write(...)");
    }

    p->win_len = temp_buf_len;
    h.index_start += temp_buf_len;
    index.push_back(p);
    return 0;
}

static int build_index(IndexFileHeader& h,photon::fs::IFile *gzfile, INDEX &index, photon::fs::IFile* index_file) {
    int32_t dict_buf_size = WINSIZE * 2;
    unsigned char *dict_buf = new unsigned char[dict_buf_size];
    DEFER(delete []dict_buf);

    int32_t inbuf_size = WINSIZE;
    unsigned char *inbuf = new unsigned char[inbuf_size];
    DEFER(delete []inbuf);

    unsigned char *window = new unsigned char[WINSIZE];
    memset(window,0, WINSIZE);
    DEFER(delete []window);

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int ret = inflateInit2(&strm, 47);
    if (ret != Z_OK) {
        return ret;
    }
    DEFER(inflateEnd(&strm));

    off_t last, ttin, ttout;
    ttin = ttout = last = strm.avail_out = 0;
    do {
        ssize_t read_cnt = gzfile->read(inbuf, inbuf_size);
        if (read_cnt < 0) {
            LOG_ERRNO_RETURN(Z_ERRNO, -1, "Failed to gzfile->read");
        }
        strm.avail_in = read_cnt;
        if (strm.avail_in == 0) {
            LOG_ERRNO_RETURN(Z_DATA_ERROR, -1, "Failed to read data");
        }
        strm.next_in = inbuf;

        do {
            if (strm.avail_out == 0) {
                strm.avail_out = WINSIZE;
                strm.next_out = window;
            }

            ttin += strm.avail_in;
            ttout += strm.avail_out;
            ret = inflate(&strm, Z_BLOCK);
            ttin -= strm.avail_in;
            ttout -= strm.avail_out;
            if (ret == Z_STREAM_END) {
                h.uncompress_file_size = ttout;
                //TODO Here generate crc32 for last uncompressed data block
                break;
            }
            if (ret != Z_OK && ret != Z_BUF_ERROR) {
                LOG_ERRNO_RETURN(0, -1, "Fail to inflate. ret:`", ret);
            }
            //TODO Here generate crc32 for uncompressed data block

            if ((strm.data_type & EACH_DEFLATE_BLOCK_BIT) && !(strm.data_type & LAST_DEFLATE_BLOCK_BIT)) {
                if (ttout == 0 || ttout - last >= h.span) {
                    if (add_index_entry(dict_buf, dict_buf_size, h, strm.data_type & 7, ttin, ttout, strm.avail_out, window, index, index_file) != 0) {
                        LOG_ERRNO_RETURN(ret, -1, "Failed to add_index_entry");
                    }
                    last = ttout;
                }
            }
        } while (strm.avail_in != 0);
    } while (ret != Z_STREAM_END);
    return 0;
}

static int get_compressed_index(const IndexFileHeader& h, const INDEX& index, unsigned char *out, int& out_len) {
	int index_len = sizeof(IndexEntry) * index.size();
	unsigned char *buf = new unsigned char[index_len];
	DEFER(delete []buf);

	IndexEntry *p = nullptr;
	for (unsigned int i=0; i<index.size(); i++) {
		p = index.at(i);
		memcpy(buf + i*sizeof(IndexEntry), p, sizeof(IndexEntry));
	}

	if (h.dict_compress_algo == 0) {
		memcpy(out, buf, index_len);
		out_len = index_len;
		return 0;
	}

	return zlib_compress(h.dict_compress_level, buf, index_len, out, out_len);
}

static int save_index_to_file(IndexFileHeader &h, INDEX& index, photon::fs::IFile *index_file) {
	int indx_cmpr_buf_len = index.size() * sizeof(IndexEntry) * 2 + 4096;
	unsigned char *buf = new unsigned char[indx_cmpr_buf_len];
	DEFER(delete []buf);

	if (get_compressed_index(h, index, buf, indx_cmpr_buf_len) != 0) {
		LOG_ERROR_RETURN(0, -1, "Failed to get_compress_index");
	}

	LOG_INFO("origin_len_of_index:`, compressed_index_len:`", index.size() * sizeof(IndexEntry), indx_cmpr_buf_len);
	if (index_file->pwrite(buf, indx_cmpr_buf_len, h.index_start) != indx_cmpr_buf_len) {
		LOG_ERROR_RETURN(0, -1, "Failed to write index, indx_cmpr_buf_len:`, h.index_start:`", indx_cmpr_buf_len, h.index_start+0);
	}
	h.index_area_len = indx_cmpr_buf_len;

	struct stat sbuf;
	if (index_file->fstat(&sbuf) != 0) {
		LOG_ERRNO_RETURN(0, -1, "Faild to index_file->fstat()");
	}
    h.index_num = index.size();
	h.index_file_size= sbuf.st_size;


    h.crc = h.cal_crc();

	if (index_file->pwrite(&h, sizeof(IndexFileHeader), 0) != sizeof(IndexFileHeader)) {
		LOG_ERROR_RETURN(0, -1, "Failed to write header");
	}
	return 0;
}

//int create_gz_index(photon::fs::IFile* gzip_file, const char *index_file_path, off_t span, unsigned char dict_compress_algo) {
//int create_gz_index(photon::fs::IFile* gzip_file, off_t span, const char *index_file_path) {
int create_gz_index(photon::fs::IFile* gzip_file, const char *index_file_path, off_t span, int dict_compress_algo, int dict_compress_level) {
	LOG_INFO("span:`,dict_compress_algo:`,dict_compress_level:`", span, dict_compress_algo, dict_compress_level);
	if (dict_compress_algo != DICT_COMPRESS_ALGO_NONE && dict_compress_algo != DICT_COMPRESS_ALGO_ZLIB) {
		LOG_ERRNO_RETURN(0, -1, "Invalid dict_compress_algo:`", dict_compress_algo);
	}

	if (dict_compress_algo == DICT_COMPRESS_ALGO_ZLIB) {
		if (dict_compress_level < -1 || dict_compress_level > 9) {
			LOG_ERRNO_RETURN(0, -1, "Invalid dict_compress_level:`, it must be in [-1, 9]", dict_compress_level);
		}
	}
	if (span < WINSIZE) {
		LOG_ERRNO_RETURN(0, -1, "Span is too small, must be greater than 100, span:`", span);
	}

	struct stat sbuf;
	if (gzip_file->fstat(&sbuf) != 0) {
		LOG_ERRNO_RETURN(0, -1, "Faild to gzip_file->fstat()");
	}

	photon::fs::IFile *index_file = photon::fs::open_localfile_adaptor(index_file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (index_file == nullptr) {
		LOG_ERROR_RETURN(0, -1, "Failed to open(`)", index_file_path);
	}
	DEFER(index_file->close());

    IndexFileHeader h;
    memset(&h, 0, sizeof(h));
    strncpy(h.magic, "ddgzidx", sizeof(h.magic));
    h.major_version =1;
    h.minor_version =0;
    h.dict_compress_algo = dict_compress_algo;
    h.dict_compress_level = dict_compress_level;
    h.flag=0;
    h.index_size = sizeof(struct IndexEntry);
    h.span = span;
    h.window= WINSIZE;
    h.gzip_file_size= sbuf.st_size;
    memset(h.reserve, 0, sizeof(h.reserve));
    h.index_start = sizeof(h);

    INDEX index;
    int ret = build_index(h, gzip_file, index, index_file);
    if (ret != 0) {
        LOG_ERRNO_RETURN(0, -1, "Faild to build_index");
    }

	ret = save_index_to_file(h, index, index_file);
    if (ret != 0) {
        LOG_ERRNO_RETURN(0, -1, "Failed to save_index_to_file(...)");
    }

    return 0;
}
