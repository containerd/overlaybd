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
#include "erofs_stress_base.h"

#define EROFS_STRESS_UNIMPLEMENTED_FUNC(ret_type, func, ret) \
ret_type func override { \
      return ret; \
}

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

TEST(ErofsStressTest, TC001) {
	std::srand(static_cast<unsigned int>(std::time(0)));
	StressCase001 *tc001 = new StressCase001("./erofs_stress_001", 20);

	ASSERT_EQ(tc001->run(), true);
	delete tc001;
}

int main(int argc, char **argv) {

	::testing::InitGoogleTest(&argc, argv);
	photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
	set_log_output_level(1);

	auto ret = RUN_ALL_TESTS();

	return ret;
}
