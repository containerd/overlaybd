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

#include <cstddef>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/photon.h>
#include "../overlaybd/lsmt/file.h"
#include "../overlaybd/zfile/zfile.h"
#include "../overlaybd/tar/libtar.h"
#include "../overlaybd/tar/erofs/liberofs.h"
#include "../overlaybd/gzindex/gzfile.h"
#include "../overlaybd/gzip/gz.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "../image_service.h"
#include "../image_file.h"
#include "CLI11.hpp"
#include "comm_func.h"

using namespace std;
using namespace photon::fs;

int dump_tar_headers(IFile *src_file, const string &out) {

    auto dst_file = open_file(out.c_str(), O_TRUNC | O_CREAT | O_RDWR, 0644);
    if (dst_file == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "create dst file failed.");
    }
    DEFER({ delete dst_file; });
    auto tar = new UnTar(src_file, nullptr, 0, 4096, nullptr, false);
    auto obj_count = tar->dump_tar_headers(dst_file);
    if (obj_count < 0) {
        return -1;
    }
    LOG_INFO("objects count: `", obj_count);
    return 0;
}

int main(int argc, char **argv) {
    std::string image_config_path, input_path, gz_index_path, config_path, fstype;
    bool raw = false, mkfs = false, verbose = false;
    bool export_tar_headers = false, import_tar_headers = false;

    CLI::App app{"this is turboOCI-apply, apply OCIv1 tar layer to 'Overlaybd-TurboOCI v1' format"};
    app.add_flag("--mkfs", mkfs, "mkfs before apply")->default_val(false);
    app.add_option("--fstype", fstype, "filesystem type")->default_val("ext4");
    app.add_flag("--verbose", verbose, "output debug info")->default_val(false);
    app.add_option("--service_config_path", config_path, "overlaybd image service config path")
        ->type_name("FILEPATH")
        ->check(CLI::ExistingFile)
        ->default_val("/etc/overlaybd/overlaybd.json");
    app.add_option("--gz_index_path", gz_index_path,
                   "build gzip index if layer is gzip, only used with turboOCI")
        ->type_name("FILEPATH")
        ->default_val("gzip.meta");
    app.add_flag("--import", import_tar_headers, "generate turboOCI file from <input_path>")
        ->default_val(false);
    app.add_flag("--export", export_tar_headers, "export tar meta from <input_path>")
        ->default_val(false);
    app.add_option("input_path", input_path, "input OCIv1 tar(gz) layer path")
        ->type_name("FILEPATH")
        ->check(CLI::ExistingFile)
        ->required();
    app.add_option("image_config_path", image_config_path, "overlaybd image config path")
        ->type_name("FILEPATH");
    //->check(CLI::ExistingFile)->required();
    CLI11_PARSE(app, argc, argv);
    set_log_output_level(verbose ? 0 : 1);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER({ photon::fini(); });

    photon::fs::IFile *src_file = nullptr;
    auto tarf = open_file(input_path.c_str(), O_RDONLY, 0666);
    if (ZFile::is_zfile(tarf) == 1) {
        tarf = ZFile::zfile_open_ro(tarf, true, true);
    }
    DEFER(delete tarf);

    if (is_gzfile(tarf)) {
        auto res = create_gz_index(tarf, gz_index_path.c_str(), 1024 * 1024);
        LOG_INFO("create_gz_index as ` `", gz_index_path, VALUE(res));
        tarf->lseek(0, 0);
        src_file = open_gzfile_adaptor(input_path.c_str());
    } else {
        src_file = tarf;
    }
    if (export_tar_headers) {
        return dump_tar_headers(src_file, image_config_path);
    }
    auto lfs = new_localfs_adaptor();
    if (lfs->access(image_config_path.c_str(), 0) != 0) {
        LOG_ERROR_RETURN(0, -1, "can't found overlaybd config: `", image_config_path);
    }

    ImageService *imgservice = nullptr;
    photon::fs::IFile *imgfile = nullptr;
    create_overlaybd(config_path, image_config_path, imgservice, imgfile);

    DEFER({
        delete imgfile;
        delete imgservice;
    });

    // for now, buffer_file can't be used with turboOCI
    if (fstype == "erofs") {
        ImageConfigNS::ImageConfig cfg;
        if (!cfg.ParseJSON(image_config_path)) {
            fprintf(stderr, "failed to parse image config\n");
            exit(-1);
        }

        auto tar = new LibErofs(imgfile, 4096, import_tar_headers);
        if (tar->extract_tar(src_file, true, cfg.lowers().size() == 0) < 0) {
            fprintf(stderr, "failed to extract\n");
            exit(-1);
        }
    } else {
        auto target = create_ext4fs(imgfile, mkfs, false, "/");
        DEFER({ delete target; });

        photon::fs::IFile *base_file = raw ? nullptr : ((ImageFile *)imgfile)->get_base();
        bool gen_turboOCI = true;
        int option = (import_tar_headers ? TAR_IGNORE_CRC : 0);
        auto tar =
            new UnTar(src_file, target, option, 4096, base_file, gen_turboOCI, import_tar_headers);
        if (tar->extract_all() < 0) {
            fprintf(stderr, "failed to extract\n");
            exit(-1);
        }
    }
    fprintf(stdout, "turboOCI-apply done\n");
    return 0;
}
