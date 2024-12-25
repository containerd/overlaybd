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

#include <gtest/gtest.h>
#include <photon/photon.h>
#include <photon/common/alog-stdstring.h>
#include <random>
#include <functional>
#include <sstream>
#include <iomanip>
#include "erofs_stress_base.h"

#define EROFS_STRESS_UNIMPLEMENTED_FUNC(ret_type, func, ret) \
ret_type func override { \
      return ret; \
}

class StressInterImpl: public StressGenInter {
public:
	/* file content */
	int max_file_size = SECTOR_SIZE * 128;
	int min_file_size = SECTOR_SIZE;
	int block_size = 4096;
	std::hash<std::string> hash_fn;
	/* xattrs */
	int xattrs_max_size = 100;
	int xattrs_min_size = 2;
	int xattrs_max_count = 10;
	int xattrs_min_count = 1;
	std::vector<std::string> xattrs_prefix = {"user."};
	char xattr_key_buffer[8192];
	char xattr_value_buffer[8192];
	/* own */
	int own_id_min = 0;
	int own_id_max = UINT32_MAX / 3;
	/* dir or file name */
	std::map<int, std::set<std::string>> name_map;

	/* generate file content in build phase */
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(min_file_size, max_file_size);
		int size =  dis(gen);
		int len;
		off64_t offset = 0;
		std::string hash_val;

		while (size > 0) {
			len = std::min(size, block_size);
			std::string block_str = get_randomstr(len, false);
			if (file->file->pwrite(block_str.c_str(), len, offset) != len)
				LOG_ERROR_RETURN(-1, -1, "fail to write to host file `", file->path);
			hash_val = std::to_string(hash_fn(hash_val + block_str));
			size -= len;
			offset += len;
		}
		node->content = hash_val;
		return true;
	}

	/* generate node content in verify phase */
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		std::string hash_val;
		char buf[block_size];
		struct stat st;
		off_t left, offset = 0;
		int len;

		if (erofs_file->fstat(&st))
				LOG_ERROR_RETURN(-1, -1, "fail to stat file");
		left = st.st_size;
		while (left > 0) {
			len = std::min((off_t)block_size, left);
			if (len != erofs_file->pread(buf, len, offset))
				LOG_ERROR_RETURN(-1, -1, "fail to pread file");

			std::string block_str(buf, len);
			hash_val = std::to_string(hash_fn(hash_val + block_str));
			left -= len;
			offset += len;
		}
		node->content = hash_val;
		return true;
	}

	/* xattrs in build phase */
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		photon::fs::IFileXAttr *xattr_ops = dynamic_cast<photon::fs::IFileXAttr*>(file->file);
		if (xattr_ops == nullptr)
			LOG_ERROR_RETURN(-1, false, "fs does not suppoert xattrs operations!");
		int xattrs_count = get_randomint(xattrs_min_count, xattrs_max_count + 1);
		for (int i = 0; i < xattrs_count; i ++) {
			int idx = get_randomint(0, xattrs_prefix.size());
			std::string key = xattrs_prefix[idx] + get_randomstr(get_randomint(xattrs_min_size, xattrs_max_size), false);
			std::string value = get_randomstr(get_randomint(xattrs_min_size, xattrs_max_size), false);
			if (xattr_ops->fsetxattr(key.c_str(), value.c_str(), value.size(), 0))
				LOG_ERROR_RETURN(-1, -1, "fail to set xattr (key: `, value: `) for file `", key, value, file->path);
			node->xattrs[key] = value;
		}
		return true;
	}

	/* xattrs in verify phase */
	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		photon::fs::IFileXAttr *xattr_ops = dynamic_cast<photon::fs::IFileXAttr*>(erofs_file);
		char *key;

		if (xattr_ops == nullptr)
			LOG_ERROR_RETURN(-1, -1, "ErofsFile doest not support xattr operations!");
		ssize_t kllen = xattr_ops->flistxattr(xattr_key_buffer, sizeof(xattr_key_buffer));
		if (kllen < 0)
			LOG_ERROR_RETURN(-1, -1, "fail to list xattrs for erofs file");
		for (key = xattr_key_buffer; key < xattr_key_buffer + kllen; key += strlen(key) + 1) {
			ssize_t value_len = xattr_ops->fgetxattr(key, xattr_value_buffer, sizeof(xattr_value_buffer));
			if (value_len < 0)
				LOG_ERROR_RETURN(-1, -1, "fail to get value for xattr `", key);
			std::string str_key = std::string(key, strlen(key));
			std::string str_value = std::string(xattr_value_buffer, value_len);
			node->xattrs[str_key] = str_value;
		}
		return true;
	}

	/* mode in build phase */
	bool build_gen_mod(StressNode *node, StressHostFile *file) override {
		std::string str_mode;
		mode_t mode;

		for (int i = 0; i < 3; i ++) {
			/* ensure that the tester can read/write the file */
			str_mode += std::to_string(get_randomint(0, 7) | (i == 0 ? 6 : 0));
		}
		mode = std::stoi(str_mode, nullptr, 8);
		if (file->file->fchmod(mode))
			LOG_ERROR_RETURN(-1, false, "fail to set mode ` for file `", str_mode, file->path);
		return true;
	}

	/* own in build phase */
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		uid_t uid = get_randomint(own_id_min, own_id_max);
		gid_t gid = get_randomint(own_id_min, own_id_max);

		meta->uid = uid;
		meta->gid = gid;
		return true;
	}

	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		size_t now = time(nullptr);
		time_t time_sec = get_randomint(now, now + 24 * 60 * 60);
		struct tm *time_info = localtime(&time_sec);
		char buffer[256];
		strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", time_info);
		std::ostringstream oss;
		oss << buffer;
		meta->mtime_date = oss.str();
		meta->mtime = time_sec;
		return true;
	}
	/* generate a random dir or file name in the current layer */
	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		std::string res;
		int cnt = 0;

		res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
		if (name_map.find(idx) ==name_map.end())
				name_map[idx] =  std::set<std::string>();
		while (name_map[idx].find(res) != name_map[idx].end()) {
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			cnt ++;
			/* try up to 1000 times */
			if (cnt > 1000)
					LOG_ERROR_RETURN(-1, "", "fail to generate a random name");
		}
		name_map[idx].insert(res);
		return res;
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		std::string str_mode("755");
		mode_t mode;

		mode = std::stoi(str_mode, nullptr, 8);
		if (host_fs->chmod(path, mode))
			LOG_ERROR_RETURN(-1, false, "fail to set mode ` for dir `", str_mode, path);
		return true;
	}

	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		uid_t uid = get_randomint(own_id_min, own_id_max);
		gid_t gid = get_randomint(own_id_min, own_id_max);

		meta->uid = uid;
		meta->gid = gid;
		return true;
	}

	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return build_gen_mtime(node, meta);
	}

	bool build_dir_xattrs(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs) override {
		photon::fs::IFileSystemXAttr *xattr_ops = dynamic_cast<photon::fs::IFileSystemXAttr*>(host_fs);
		if (xattr_ops == nullptr)
			LOG_ERROR_RETURN(-1, false, "fs does not suppoert xattrs operations!");
		int xattrs_count = get_randomint(xattrs_min_count, xattrs_max_count + 1);
		for (int i = 0; i < xattrs_count; i ++) {
			int idx = get_randomint(0, xattrs_prefix.size());
			std::string key = xattrs_prefix[idx] + get_randomstr(get_randomint(xattrs_min_size, xattrs_max_size), false);
			std::string value = get_randomstr(get_randomint(xattrs_min_size, xattrs_max_size), false);
			if (xattr_ops->setxattr(path, key.c_str(), value.c_str(), value.size(), 0))
				LOG_ERROR_RETURN(-1, -1, "fail to set xattr (key: `, value: `) for dir `", key, value, path);
			node->xattrs[key] = value;
		}
		return true;
	}

	bool build_stat_file(StressNode *node, StressHostFile *file_info, struct in_mem_meta *meta) override {
		if (file_info->file->fstat(&node->node_stat))
			LOG_ERRNO_RETURN(-1, false, "fail to stat file `", file_info->path);
		node->node_stat.st_uid = meta->uid;
		node->node_stat.st_gid = meta->gid;
		node->node_stat.st_mtime = meta->mtime;

		return true;
	}

	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		if (host_fs->stat(path, &node->node_stat))
			LOG_ERROR_RETURN(-1, false, "fail to stat dir `", path);
		node->node_stat.st_uid = meta->uid;
		node->node_stat.st_gid = meta->gid;
		node->node_stat.st_mtime = meta->mtime;
		return true;
	}

	bool verify_stat(StressNode *node,  photon::fs::IFile *erofs_file) override {
		if (erofs_file->fstat(&node->node_stat))
			LOG_ERRNO_RETURN(-1, false, "fail to stat erofs_file");
		return true;
	}
};

/*
 * TC001
 *
 * Create 20 layers, each layer contains a tree with 2 dirs,
 * each dir contains 50 empty files.
 *
 * A simple test for verifying the integrity of the FS tree.
 */
class StressCase001: public StressBase, public StressInterImpl {
public:
	StressCase001(std::string path, int layers): StressBase(path, layers) {}

	/* create empty files in build phase*/
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_mod(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_xattrs(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_content(StressNode *node, StressHostFile *file), true)
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return node->type == NODE_DIR ? StressInterImpl::verify_gen_xattrs(node, erofs_file): true;
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}

	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)

	/* simplely generate random dir and file names */
	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		return StressInterImpl::generate_name(idx, depth, root_path, type);
	}

	/*
	 * each layer has two dirs:
	 * dir one contains 50 files,
	 * dir two contains 50 files
	 */
	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		ret.emplace_back(3);
		ret.emplace_back(3);
		return ret;
	}
};

/*
 * TC002
 *
 * Create layers, each layer contains 2 dirs,
 * each dir contains 10 files.
 *
 * Testing the integrity of file contents.
 */
class StressCase002: public StressBase, public StressInterImpl {
public:
	StressCase002(std::string path, int layers): StressBase(path, layers) {}

	/* leave mod/own/xattr empty */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_mod(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_xattrs(StressNode *node, StressHostFile *file), true)
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_content(node, file);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return node->type == NODE_DIR ? StressInterImpl::verify_gen_xattrs(node, erofs_file): true;
	}
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_content(node, erofs_file);
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		return StressInterImpl::generate_name(idx, depth, root_path, type);
	}

	/*
	 * each layer has two dirs:
	 * dir one contains 10 files,
	 * dir two contains 10 files
	 */
	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		ret.emplace_back(10);
		ret.emplace_back(10);
		return ret;
	}
};

/*
 * TC003
 *
 * Create layers, each layer contains 10 dirs,
 * each dir contains 10 files.
 *
 * Testing the xattrs of files.
 */
class StressCase003: public StressBase, public StressInterImpl {
public:
	StressCase003(std::string path, int layers): StressBase(path, layers) {}


	/* leave mod/own/content empty */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_mod(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_content(StressNode *node, StressHostFile *file), true)
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_xattrs(node, file);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_xattrs(node, erofs_file);
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)

	/* simplely generate random dir and file names */
	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		return StressInterImpl::generate_name(idx, depth, root_path, type);
	}

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		/* 10 dirs, each dir contains 10 files */
		for (int i = 0; i < 10; i ++)
			ret.emplace_back(10);
		return ret;
	}

};

/*
 * TC004
 *
 * Create layers, each layer contains 10 dirs,
 * each dir contains 10 files.
 *
 * Testing the mode of files.
 */
class StressCase004: public StressBase, public StressInterImpl {
public:
	StressCase004(std::string path, int layers): StressBase(path, layers) {}

	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_xattrs(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_content(StressNode *node, StressHostFile *file), true)
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_gen_mod(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_mod(node, file);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)
	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return node->type == NODE_DIR ? StressInterImpl::verify_gen_xattrs(node, erofs_file): true;
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		return StressInterImpl::generate_name(idx, depth, root_path, type);
	}

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		/* 10 dirs, each dir contains 10 files */
		for (int i = 0; i < 10; i ++)
			ret.emplace_back(10);
		return ret;
	}
};

/*
 * TC005
 *
 * Create layers, each layer contains 10 dirs,
 * each dir contains 10 files.
 *
 * Testing the uid/gid of files.
 */
class StressCase005: public StressBase, public StressInterImpl {
public:
	StressCase005(std::string path, int layers): StressBase(path, layers) {}

	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_mod(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_xattrs(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_content(StressNode *node, StressHostFile *file), true)
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return node->type == NODE_DIR ? StressInterImpl::verify_gen_xattrs(node, erofs_file): true;
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)

	/* simplely generate random dir and file names */
	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		return StressInterImpl::generate_name(idx, depth, root_path, type);
	}

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		/* 10 dirs, each dir contains 10 files */
		for (int i = 0; i < 10; i ++)
			ret.emplace_back(10);
		return ret;
	}
};

/*
 * TC006
 *
 * Create layers, each layer contains 10 dirs,
 * each dir contains 10 files.
 *
 * Testing the mode, uid/gid, xattrs, content of files.
 */
class StressCase006: public StressBase, public StressInterImpl {
public:
	StressCase006(std::string path, int layers): StressBase(path, layers) {}

	bool build_gen_mod(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_mod(node, file);
	}
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_xattrs(node, file);
	}
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_content(node, file);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_xattrs(node, erofs_file);
	}
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_content(node, erofs_file);
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		return StressInterImpl::generate_name(idx, depth, root_path, type);
	}

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		for (int i = 0; i < 10; i ++)
			ret.emplace_back(10);
		return ret;
	}
};

/*
 * TC007
 *
 * Create layers, each layer contains 10 dirs,
 * each dir contains 10 files.
 *
 * Test the scenario where the upper layer and lower
 * layer contain files or directories with the same name.
 */
class StressCase007: public StressBase, public StressInterImpl {
private:
	std::map<int, std::set<std::string>> mp;
public:
	StressCase007(std::string path, int layers): StressBase(path, layers) {}

	bool build_gen_mod(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_mod(node, file);
	}
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_xattrs(node, file);
	}
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_content(node, file);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_xattrs(node, erofs_file);
	}
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_content(node, erofs_file);
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}

	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		std::string res;
		int cnt = 0;

		if (idx < 1) {
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			goto out;
		}
		res = tree->get_same_name(idx, depth, root_path, type);
		/* fall back to a random name */
		if (res.length() == 0)
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
		if (mp.find(idx) == mp.end())
			mp[idx] = std::set<std::string>();
		/* already used in this layer, fall back to a random name */
		while (mp[idx].find(res) != mp[idx].end()) {
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			cnt ++;
			if (cnt > 1000)
				LOG_ERROR_RETURN(-1, "", "fail to gernate name in TC007");
		}
		mp[idx].insert(res);
	out:
		return res;
	}

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		for (int i = 0; i < 10; i ++)
			ret.emplace_back(30);
		return ret;
	}
};

/*
 * TC008
 *
 * Create layers, each layer contains 50 dirs,
 * each dir contains 2 files.
 *
 * Test whiteout files.
 */
class StressCase008: public StressBase, public StressInterImpl {
private:
	std::map<int, std::set<std::string>> mp;
public:
	StressCase008(std::string path, int layers): StressBase(path, layers) {}

	bool build_gen_mod(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_mod(node, file);
	}
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_xattrs(node, file);
	}
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_content(node, file);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_xattrs(node, erofs_file);
	}
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_content(node, erofs_file);
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}

	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		std::string res;
		int cnt = 0;

		if (idx < 1) {
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			goto out;
		}

		if (idx & 1) {
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			goto verify_str;
		} else {
			/* generate whiteout files at even layers */
			res = tree->get_same_name(idx, depth, root_path, type, true);
			/* fall back to a random name */
			if (res.length() == 0)
				res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
		}
verify_str:
		if (mp.find(idx) == mp.end())
			mp[idx] = std::set<std::string>();
		/* already used in this layer, fall back to a random name */
		while (mp[idx].find(res) != mp[idx].end()) {
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			cnt ++;
			if (cnt > 1000)
				LOG_ERROR_RETURN(-1, "", "fail to gernate name");
		}
		mp[idx].insert(res);
		if (tree->get_type(root_path + "/" + res) == type && depth > 0)
			res = std::string(EROFS_WHOUT_PREFIX) + res; /* .wh. file */
	out:
		return res;
	}

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		/* 50 dirs, each contains 2 files */
		for (int i = 0; i < 50; i ++)
			ret.emplace_back(2);
		return ret;
	}
};

/*
 * TC009
 *
 * Test the scenario of deleting first and then creating files/dirs.
 */
class StressCase009: public StressBase, public StressInterImpl {
private:
	std::map<int, std::set<std::string>> mp;
	std::set<std::string> deleted_names;
public:
	StressCase009(std::string path, int layers): StressBase(path, layers) {}

	bool build_gen_mod(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_mod(node, file);
	}
	bool build_gen_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_own(node, meta);
	}
	bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_gen_mtime(node, meta);
	}
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_xattrs(node, file);
	}
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_content(node, file);
	}
	bool build_stat_file(StressNode *node, StressHostFile *file, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_file(node, file, meta);
	}

	bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_mod(node, path, host_fs);
	}
	bool build_dir_own(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_own(node, meta);
	}
	bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) override {
		return StressInterImpl::build_dir_mtime(node, meta);
	}
	bool build_dir_xattrs(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) override {
		return StressInterImpl::build_dir_xattrs(node, path, host_fs);
	}
	bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) override {
		return StressInterImpl::build_stat_dir(node, path, host_fs, meta);
	}

	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_xattrs(node, erofs_file);
	}
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_content(node, erofs_file);
	}
	bool verify_stat(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_stat(node, erofs_file);
	}

	std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) override {
		std::string res;
		int cnt = 0;

		if (mp.find(idx) == mp.end())
			mp[idx] = std::set<std::string>();

		if (idx == 0)
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
		else if (idx == 1) {
			res = tree->get_same_name(idx, depth, root_path, type, true);
			if (res.length() == 0)
				res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			/* if it is already been used, then generate a random name */
			while (mp[idx].find(res) != mp[idx].end()) {
				res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
				cnt ++;
				if (cnt > 1000)
					LOG_ERROR_RETURN(-1, "", "fail to generate name");
			}
			mp[idx].insert(res);
			if (tree->get_type(root_path + "/" + res) == type && depth > 0) {
				deleted_names.insert(root_path + "/" + res);
				LOG_INFO("delete file/dir: `, type: `", res, tree->get_type(root_path + "/" + res));
				res = std::string(EROFS_WHOUT_PREFIX) + res;
			}
		} else if (idx == 2) {
			if (depth == 0)
				res = tree->get_same_name(idx, depth, root_path, type, true);
			else {
				root_path += "/";
				for (const std::string& name: deleted_names) {
					if (str_n_equal(name, root_path, root_path.length())) {
						std::string last_component = name.substr(root_path.length());
						if (last_component.length() > 0 && !is_substring(last_component, "/")) {
							if (mp[idx].find(last_component) == mp[idx].end()) {
								res = last_component;
								LOG_INFO("find deleted name: `, reuse it", res);
								break;
							}
						}
					}
				}
			}
			if (res.length() == 0)
				res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
			goto check_res;
		} else {
			res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
	check_res:	while (mp[idx].find(res) != mp[idx].end()) {
				res = get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);
				cnt ++;
				if (cnt > 1000)
					LOG_ERROR_RETURN(-1, "", "fail to generate name");
			}
			mp[idx].insert(res);
		}
		return res;
	}

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		/* 1000 dirs, each contains 2 files */
		for (int i = 0; i < 1000; i ++)
			ret.emplace_back(2);
		return ret;
	}
};

TEST(ErofsStressTest, TC001) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	StressCase001 *tc001 = new StressCase001("./erofs_stress_001", 20);

	ASSERT_EQ(tc001->run(), true);
	delete tc001;
}

TEST(ErofsStressTest, TC002) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	StressCase002 *tc002 = new StressCase002("./erofs_stress_002", 10);

	ASSERT_EQ(tc002->run(), true);
	delete tc002;
}

TEST(ErofsStressTest, TC003) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	StressCase003 *tc003 = new StressCase003("./erofs_stress_003", 20);

	ASSERT_EQ(tc003->run(), true);
	delete tc003;
}

TEST(ErofsStressTest, TC004) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	StressCase004 *tc004 = new StressCase004("./erofs_stress_004", 10);

	ASSERT_EQ(tc004->run(), true);
	delete tc004;
}

TEST(ErofsStressTest, TC005) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	StressCase005 *tc005 = new StressCase005("./erofs_stress_005", 10);

	ASSERT_EQ(tc005->run(), true);
	delete tc005;
}

TEST(ErofsStressTest, TC006) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	/* 30 layers */
	StressCase006 *tc006 = new StressCase006("./erofs_stress_006", 30);

	ASSERT_EQ(tc006->run(), true);
	delete tc006;
}

TEST(ErofsStressTest, TC007) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	/* 50 layers */
	StressCase007 *tc007 = new StressCase007("./erofs_stress_007", 50);

	ASSERT_EQ(tc007->run(), true);
	delete tc007;
}

TEST(ErofsStressTest, TC008) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	/* 30 layers */
	StressCase008 *tc008 = new StressCase008("./erofs_stress_008", 30);

	ASSERT_EQ(tc008->run(), true);
	delete tc008;
}

TEST(ErofsStressTest, TC009) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	StressCase009 *tc009 = new StressCase009("./erofs_stress_009", 3);

	ASSERT_EQ(tc009->run(), true);
	delete tc009;
}

int main(int argc, char **argv) {

	::testing::InitGoogleTest(&argc, argv);
	photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
	set_log_output_level(1);

	auto ret = RUN_ALL_TESTS();

	return ret;
}
