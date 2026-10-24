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
#include <photon/fs/virtual-file.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/photon.h>
#include "../overlaybd/lsmt/file.h"
#include "../overlaybd/zfile/zfile.h"
#include "../overlaybd/tar/libtar.h"
#include "../overlaybd/gzindex/gzfile.h"
#include "../overlaybd/gzip/gz.h"
#include "../overlaybd/zstd/zstdfile.h"
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
#include "../image_file.h"
#include "CLI11.hpp"
#include "comm_func.h"
#include "sha256file.h"
#include "qcow2converter.h"
#include "../overlaybd/tar/fsmeta_file.h"


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

// the file is gone as soon as it is closed
static photon::fs::IFile *create_temp_file() {
    auto dir = getenv("TMPDIR");
    std::string path = std::string(dir && *dir ? dir : "/tmp") + "/overlaybd-apply-XXXXXX";
    int fd = mkstemp(&path[0]);
    if (fd < 0) {
        LOG_ERRNO_RETURN(0, nullptr, "failed to create temporary file `", path);
    }
    unlink(path.c_str());
    auto file = new_localfile_adaptor(fd);
    if (file == nullptr) {
        close(fd);
    }
    return file;
}

int main(int argc, char **argv) {
    std::string image_config_path, input_path, gz_index_path, config_path, sha256_checksum;
    string tarheader;
    bool raw = false, mkfs = false, verbose = false, qcow2 = false, extract_rootfs = true;

    CLI::App app{"this is overlaybd-apply, apply OCIv1 tar layer or qcow2 image to overlaybd format"};
    app.add_flag("--raw", raw, "apply to raw image")->default_val(false);
    app.add_flag("--mkfs", mkfs, "mkfs before apply")->default_val(false);
    app.add_flag("--from_qcow2", qcow2, "convert from qcow2 image file instead of tar")->default_val(false);
    app.add_flag("--extract-rootfs,!--no-extract-rootfs", extract_rootfs,
                 "Only extract rootfs partitions (default: on). "
                 "Use --no-extract-rootfs to convert entire disk.");

    app.add_flag("--verbose", verbose, "output debug info")->default_val(false);
    app.add_option("--service_config_path", config_path, "overlaybd image service config path")->type_name("FILEPATH")->check(CLI::ExistingFile)->default_val("/etc/overlaybd/overlaybd.json");
    app.add_option("--gz_index_path", gz_index_path, "build gzip index if layer is gzip, only used with turboOCIv1")->type_name("FILEPATH");
    app.add_option("--checksum", sha256_checksum, "sha256 checksum for origin uncompressed data");
    app.add_option("input_path", input_path, "input OCIv1 tar layer path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    app.add_option("image_config_path", image_config_path, "overlaybd image config path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    CLI11_PARSE(app, argc, argv);

    set_log_output_level(verbose ? 0 : 1);
    photon::init(photon_event_engine_from_config(config_path), photon::INIT_IO_DEFAULT);
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

    if(qcow2){
        int ret = convert_qcow2_to_imgfile(input_path.c_str(), imgfile,
                                            DEFAULT_BLOCK_SIZE, verbose, extract_rootfs);
        if (ret != 0) {
            fprintf(stderr, "qcow2 conversion failed\n");
            return -1;
        }
        printf("qcow2 to overlaybd conversion done\n");
        return 0;
    }

    bool gen_turboOCI = (gz_index_path != "" );
    photon::fs::IFile* base_file = raw ? nullptr : ((ImageFile *)imgfile)->get_base();
    // a plain append-mode layer gets all the file data ahead of the fs metadata
    auto staging = gen_turboOCI ? nullptr : dynamic_cast<LSMT::IFileRW *>(base_file);
    if (staging && staging->can_map_staged_data() != 0) {
        staging = nullptr;
    }

    std::unique_ptr<photon::fs::IFile> fsmeta_sparse;
    std::unique_ptr<FsMetaFile> fsmeta;
    // with staging, the fs (mkfs included) is created once the file data is in place
    photon::fs::IFileSystem *target =
        staging ? nullptr : create_ext4fs(imgfile, mkfs, !gen_turboOCI);
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
    } else if (is_zstdfile(tarf)) {
        src_file = open_zstdfile_adaptor(tarf, /*ownership=*/false);
    } else {
        src_file = tarf;
    }

    if (!sha256_checksum.empty()) {
        src_file = checksum_file = new_sha256_file(src_file, true);
    }

    auto verify_checksum = [&]() {
        if (checksum_file != nullptr) {
            auto calc = checksum_file->sha256_checksum();
            if (calc != sha256_checksum) {
                fprintf(stderr, "sha256 checksum mismatch, expect: %s, got: %s\n", sha256_checksum.c_str(), calc.c_str());
                exit(-1);
            }
        }
    };

    if (staging) {
        auto headers = create_temp_file();
        if (headers == nullptr) {
            fprintf(stderr, "failed to create a temporary file\n");
            exit(-1);
        }
        DEFER(delete headers);
        UnTar scan(src_file, nullptr, 0, 4096);
        if (scan.stage_tar(headers, staging->get_lower_files().back()) < 0) {
            fprintf(stderr, "failed to stage file data\n");
            exit(-1);
        }
        verify_checksum();
        if (headers->lseek(0, SEEK_SET) != 0) {
            fprintf(stderr, "failed to rewind the tar headers\n");
            exit(-1);
        }
        fsmeta_sparse.reset(create_temp_file());
        struct stat image_st;
        if (fsmeta_sparse == nullptr || imgfile->fstat(&image_st) != 0) {
            fprintf(stderr, "failed to create fs metadata staging file\n");
            return -1;
        }
        fsmeta.reset(new FsMetaFile(imgfile, fsmeta_sparse.get(), image_st.st_size));
        target = create_ext4fs(fsmeta.get(), mkfs, false);
        UnTar replay(headers, target, 0, 4096, base_file);
        if (replay.extract_staged(fsmeta.get(), [&]() {
                delete target;
                target = nullptr;
            }) < 0) {
            fprintf(stderr, "failed to extract\n");
            return -1;
        }
    } else {
        UnTar tar(src_file, target, 0, 4096, base_file, gen_turboOCI);
        if (tar.extract_all() < 0) {
            fprintf(stderr, "failed to extract\n");
            exit(-1);
        }
        verify_checksum();
    }
    fprintf(stdout, "overlaybd-apply done\n");
    fprintf(stderr, "%s\n",  sha256_checksum.c_str());

    return 0;
}
