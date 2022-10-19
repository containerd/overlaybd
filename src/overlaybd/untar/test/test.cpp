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

#include <vector>
#include <iostream>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <gflags/gflags.h>
#include <photon/photon.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/aligned-file.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>
#include "../libtar.h"


TEST(UntarTest, basic) {
    auto tarf = photon::fs::open_localfile_adaptor("/root/tartest/mkwh/wh1.tar", O_RDONLY, 0666, 0);
    auto target = photon::fs::new_localfs_adaptor("/root/tartest/rootfs");
    auto tar = new Tar(tarf, target, 0);
    tar->extract_all();
}

int main(int argc, char **argv) {

    ::testing::InitGoogleTest(&argc, argv);
    ::gflags::ParseCommandLineFlags(&argc, &argv, true);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    return 0;
}