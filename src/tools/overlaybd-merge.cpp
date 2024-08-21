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
    std::string image_config_path, input_path, output, config_path, sha256_checksum;
    string tarheader;
    bool zfile = false, mkfs = false, verbose = false, raw = false;

    CLI::App app{"this is overlaybd-merge, merge multiple overlaybd layers into a single."};

    app.add_flag("--verbose", verbose, "output debug info")->default_val(false);
    app.add_option("--service_config_path", config_path, "overlaybd image service config path")->type_name("FILEPATH")->check(CLI::ExistingFile)->default_val("/etc/overlaybd/overlaybd.json");
    app.add_flag("--compress", zfile, "do zfile compression for the output layer")->default_val(true);

    app.add_option("image_config_path", image_config_path, "overlaybd image config path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    app.add_option("output", output, "compacted layer path")->type_name("FILEPATH");

    CLI11_PARSE(app, argc, argv);

    set_log_output_level(verbose ? 0 : 1);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER({photon::fini();});


    ImageService *imgservice = nullptr;
    photon::fs::IFile *imgfile = nullptr;
    create_overlaybd(config_path, image_config_path, imgservice, imgfile);
    if (imgfile == nullptr) {
        fprintf(stderr, "failed to create image file\n");
        exit(-1);
    }
     DEFER({
        delete imgfile;
        delete imgservice;
    });
    auto rst = open_localfile_adaptor(output.c_str(), O_CREAT|O_TRUNC|O_RDWR, 0644);
    if (rst == nullptr){
        fprintf(stderr, "failed to create output file\n");
        exit(-1);
    }
    DEFER(delete rst);
    if (zfile) {
        rst = ZFile::new_zfile_builder(rst);
        if (rst == nullptr) {
            fprintf(stderr, "failed to create zfile\n");
            exit(-1);
        }
    }
    if (((ImageFile*)imgfile)->compact(rst)!=0){
        fprintf(stderr, "failed to compact\n");
        exit(-1);
    }

    return 0;
}
