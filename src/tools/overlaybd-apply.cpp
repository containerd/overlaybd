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

#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/fs/subfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/photon.h>
#include "../overlaybd/lsmt/file.h"
#include "../overlaybd/zfile/zfile.h"
#include "../overlaybd/tar/libtar.h"
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
#include "sha256file.h"

using namespace std;
using namespace photon::fs;

class FIFOFile : public VirtualReadOnlyFile {
public:
    IFile *m_fifo;

    FIFOFile(IFile *fifo): m_fifo(fifo) { }
    ~FIFOFile() { delete m_fifo; }

    ssize_t read(void *buf, size_t count) override {
        size_t left = count;
        char *pos = (char *) buf;
        LOG_DEBUG(VALUE(count));
        while (left > 0) {
            ssize_t readn = m_fifo->read(pos, left);
            if (readn < 0 || readn > (ssize_t) left) {
                LOG_ERRNO_RETURN(0, -1, "failed to read fifo", VALUE(left), VALUE(readn));
            }
            left -= (size_t) readn;
            pos += readn;
            LOG_DEBUG("fifo read", VALUE(readn));
        }
        LOG_DEBUG(VALUE(left));
        return count - left;
    }

    int fstat(struct stat *buf) override {
        return m_fifo->fstat(buf);
    }

    UNIMPLEMENTED_POINTER(IFileSystem *filesystem() override);
    UNIMPLEMENTED(off_t lseek(off_t offset, int whence) override);
    UNIMPLEMENTED(ssize_t readv(const struct iovec *iov, int iovcnt) override);
    UNIMPLEMENTED(ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override);
};

int main(int argc, char **argv) {
    std::string image_config_path, input_path, gz_index_path, config_path, sha256_checksum;
    string tarheader;
    bool raw = false, mkfs = false, verbose = false;

    CLI::App app{"this is overlaybd-apply, apply OCIv1 tar layer to overlaybd format"};
    app.add_flag("--raw", raw, "apply to raw image")->default_val(false);
    app.add_flag("--mkfs", mkfs, "mkfs before apply")->default_val(false);

    app.add_flag("--verbose", verbose, "output debug info")->default_val(false);
    app.add_option("--service_config_path", config_path, "overlaybd image service config path")->type_name("FILEPATH")->check(CLI::ExistingFile)->default_val("/etc/overlaybd/overlaybd.json");
    app.add_option("--gz_index_path", gz_index_path, "build gzip index if layer is gzip, only used with turboOCIv1")->type_name("FILEPATH");
    app.add_option("--checksum", sha256_checksum, "sha256 checksum for origin uncompressed data");
    app.add_option("input_path", input_path, "input OCIv1 tar layer path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();

    app.add_option("image_config_path", image_config_path, "overlaybd image config path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    CLI11_PARSE(app, argc, argv);

    set_log_output_level(verbose ? 0 : 1);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER({photon::fini();});


    ImageService *imgservice = nullptr;
    photon::fs::IFile *imgfile = nullptr;
    if (raw) {
        imgfile = open_file(image_config_path.c_str(), O_RDWR, 0644);
    } else {
        create_overlaybd(config_path, image_config_path, imgservice, imgfile);
    }
    if (imgfile == nullptr) {
        fprintf(stderr, "failed to create image file\n");
        exit(-1);
    }
    DEFER({
        delete imgfile;
        delete imgservice;
    });
    bool gen_turboOCI = (gz_index_path != "" );

    auto target = create_ext4fs(imgfile, mkfs, !gen_turboOCI, "/");
    DEFER({ delete target; });

    photon::fs::IFile* src_file = nullptr;
    SHA256File* checksum_file = nullptr;

    auto tarf = open_file(input_path.c_str(), O_RDONLY, 0666);
    DEFER(delete tarf);

    struct stat st;
    auto ret = tarf->fstat(&st);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "failed to stat `", input_path, VALUE(ret));
    }
    if (S_ISFIFO(st.st_mode)) {
        src_file = new FIFOFile(tarf);
    } else if (is_gzfile(tarf)) {
        if (gz_index_path != "") {
            auto res = create_gz_index(tarf, gz_index_path.c_str(), 1024*1024);
            LOG_INFO("create_gz_index ", VALUE(res));
            tarf->lseek(0, 0);
        }
        src_file = open_gzfile_adaptor(input_path.c_str());
    } else {
        src_file = tarf;
    }

    if (!sha256_checksum.empty()) {
        src_file = checksum_file = new_sha256_file(src_file, true);
    }

    photon::fs::IFile* base_file = raw ? nullptr : ((ImageFile *)imgfile)->get_base();

    auto tar = new UnTar(src_file, target, 0, 4096, base_file, gen_turboOCI);

    if (tar->extract_all() < 0) {
        fprintf(stderr, "failed to extract\n");
        exit(-1);
    } else {
        if (checksum_file != nullptr) {
            auto calc = checksum_file->sha256_checksum();
            if (calc != sha256_checksum) {
                fprintf(stderr, "sha256 checksum mismatch, expect: %s, got: %s\n", sha256_checksum.c_str(), calc.c_str());
                exit(-1);
            }
        }
        fprintf(stdout, "overlaybd-apply done\n");
        fprintf(stderr, "%s\n",  sha256_checksum.c_str());
    }

    return 0;
}
