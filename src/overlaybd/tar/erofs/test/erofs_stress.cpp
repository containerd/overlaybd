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
			str_mode += std::to_string(get_randomint(0, 7));
		}
		mode = std::stoi(str_mode, nullptr, 8);
		if (file->file->fchmod(mode))
			LOG_ERROR_RETURN(-1, false, "fail to set mode ` for file `", str_mode, file->path);
		node->mod = str_mode;
		return true;
	}

	/* mode in verify phase */
	bool verify_gen_mod(StressNode *node, photon::fs::IFile *erofs_file) override {
		struct stat st;
		std::ostringstream oss;

		if (erofs_file->fstat(&st))
			LOG_ERROR_RETURN(-1, false, "fail to stat erofs file");
		oss << std::oct << std::setfill('0') << std::setw(3) << (st.st_mode & 0777);
		node->mod = oss.str();
		return true;
	}

	/* own in build phase */
	bool build_gen_own(StressNode *node, StressHostFile *file) override {
		uid_t uid = get_randomint(own_id_min, own_id_max);
		gid_t gid = get_randomint(own_id_min, own_id_max);

		if (file->file->fchown(uid, gid))
			LOG_ERROR_RETURN(-1,false, "failt to chown of file `", file->path);
		node->own = std::to_string(uid) + std::to_string(gid);
		return true;
	}

	/* own in verify phase */
	bool verify_gen_own(StressNode *node, photon::fs::IFile *erofs_file) override {
		struct stat st;

		if (erofs_file->fstat(&st))
			LOG_ERROR_RETURN(-1, false, "fail to stat erofs file");
		node->own = std::to_string(st.st_uid) + std::to_string(st.st_gid);
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
class StressCase001: public StressBase {
public:
	StressCase001(std::string path, int layers): StressBase(path, layers) {}

	/* create empty files in build phase*/
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_mod(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_own(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_xattrs(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_content(StressNode *node, StressHostFile *file), true)

	/* create empty nodes in verify phase */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_mod(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_own(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)

	/* simplely generate random dir and file names */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(std::string, generate_name(int idx, int depth, std::string _prefix, NODE_TYPE type), \
				        get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true))

	/*
	 * each layer has two dirs:
	 * dir one contains 50 files,
	 * dir two contains 50 files
	 */
	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		ret.emplace_back(50);
		ret.emplace_back(50);
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
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_own(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_xattrs(StressNode *node, StressHostFile *file), true)
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_content(node, file);
	}

	/* leave mod/own/xattr empty */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_mod(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_own(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file), true)
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_content(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(std::string, generate_name(int idx, int depth, std::string _prefix, NODE_TYPE type), \
				        get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true))

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
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_own(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_content(StressNode *node, StressHostFile *file), true)
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_xattrs(node, file);
	}

	/* leave mod/own/content empty */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_mod(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_own(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)
	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_xattrs(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(std::string, generate_name(int idx, int depth, std::string _prefix, NODE_TYPE type), \
				        get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true))

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

	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_own(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_xattrs(StressNode *node, StressHostFile *file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, build_gen_content(StressNode *node, StressHostFile *file), true)
	bool build_gen_mod(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_mod(node, file);
	}

	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_own(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)
	bool verify_gen_mod(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_mod(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(std::string, generate_name(int idx, int depth, std::string _prefix, NODE_TYPE type), \
				        get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true))

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
	bool build_gen_own(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_own(node, file);
	}

	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_mod(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file), true)
	EROFS_STRESS_UNIMPLEMENTED_FUNC(bool, verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file), true)
	bool verify_gen_own(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_own(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(std::string, generate_name(int idx, int depth, std::string _prefix, NODE_TYPE type), \
				        get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true))

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
	bool build_gen_own(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_own(node, file);
	}
	bool build_gen_xattrs(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_xattrs(node, file);
	}
	bool build_gen_content(StressNode *node, StressHostFile *file) override {
		return StressInterImpl::build_gen_content(node, file);
	}

	bool verify_gen_mod(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_mod(node, erofs_file);
	}
	bool verify_gen_own(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_own(node, erofs_file);
	}
	bool verify_gen_xattrs(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_xattrs(node, erofs_file);
	}
	bool verify_gen_content(StressNode *node, photon::fs::IFile *erofs_file) override {
		return StressInterImpl::verify_gen_content(node, erofs_file);
	}

	/* simplely generate random dir and file names */
	EROFS_STRESS_UNIMPLEMENTED_FUNC(std::string, generate_name(int idx, int depth, std::string _prefix, NODE_TYPE type), \
				        get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true))

	std::vector<int> layer_dirs(int idx) {
		std::vector<int> ret;

		for (int i = 0; i < 10; i ++)
			ret.emplace_back(10);
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

int main(int argc, char **argv) {

	::testing::InitGoogleTest(&argc, argv);
	photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
	set_log_output_level(1);

	auto ret = RUN_ALL_TESTS();

	return ret;
}
