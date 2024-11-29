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
#include "erofs_stress_base.h"

#define EROFS_STRESS_UNIMPLEMENTED_FUNC(ret_type, func, ret) \
ret_type func override { \
      return ret; \
}

class StressInterImpl: public StressGenInter {
public:
	int max_file_size = SECTOR_SIZE * 128;
	int min_file_size = SECTOR_SIZE;
	int block_size = 4096;
	std::hash<std::string> hash_fn;

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

int main(int argc, char **argv) {

	::testing::InitGoogleTest(&argc, argv);
	photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
	set_log_output_level(1);

	auto ret = RUN_ALL_TESTS();

	return ret;
}
