#pragma once
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include "photon/common/checksum/crc32c.h"
#define WINSIZE 32768U
#define GZFILE_INDEX_MAGIC "ddgzidx"

struct IndexFileHeader {
	char magic[8];
	uint8_t major_version;
	uint8_t minor_version;
	uint8_t dict_compress_algo;
	int8_t dict_compress_level;
	uint8_t flag;
	int32_t span;
	int32_t window;
	int32_t index_size;
	int64_t index_num;
	int64_t gzip_file_size;
	int64_t index_file_size;
	int64_t uncompress_file_size;
	char reserve[256];
	int64_t index_start;
	int64_t index_area_len;
	uint32_t crc;
	uint32_t cal_crc() { return crc32c(this, sizeof(IndexFileHeader) - sizeof(crc));}
	std::string to_str() {
		std::stringstream ss;
		ss  << "magic:" << magic
			<<",major_version:"<<static_cast<int32_t>(major_version)
			<<",minor_version:"<<static_cast<int32_t>(minor_version)
			<<",dict_compress_algo:"<<static_cast<int32_t>(dict_compress_algo)
			<<",dict_compress_level:"<<static_cast<int32_t>(dict_compress_level)
			<<",flag:" <<static_cast<int32_t>(flag)
			<<",span:" <<span
			<<",window:" <<window
			<<",index_size:" <<index_size
			<<",index_num:" <<index_num
			<<",gzip_file_size:" <<gzip_file_size
			<<",index_file_size:" <<index_file_size
			<<",reserve[256]:" <<reserve
			<<",index_start:" <<index_start
			<<",index_area_len:" <<index_area_len
			<<",crc:" <<crc;

		return ss.str();
	}
} __attribute__((packed));

struct IndexEntry {
	off_t de_pos;
	off_t en_pos;
	off_t win_pos;
	uint8_t bits;
	uint32_t win_len;
}__attribute__((packed));


typedef std::vector<struct IndexEntry *> INDEX;
