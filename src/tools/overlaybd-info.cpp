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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <memory>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../overlaybd/fs/tar_file.h"

#include "../overlaybd/fs/lsmt/file.h"
#include "../overlaybd/fs/localfs.h"
#include "../overlaybd/utility.h"
#include "../overlaybd/alog.h"
#include "../overlaybd/alog-stdstring.h"
#include "../overlaybd/fs/zfile/zfile.h"
#include "../overlaybd/fs/registryfs/registryfs.h"
#include "../overlaybd/net/curl.h"
#include "../overlaybd/photon/thread.h"
#include "../overlaybd/photon/syncio/fd-events.h"
#include "../image_service.h"

using namespace std;
using namespace LSMT;
using namespace FileSystem;

struct HeaderTrailer {
    static const uint32_t SPACE = 4096;
    static const uint32_t TAG_SIZE = 256;
    static uint64_t MAGIC0() {
        static char magic0[] = "LSMT\0\1\2";
        return *(uint64_t *)magic0;
    }
    static constexpr UUID MAGIC1() {
        return {0xd2637e65, 0x4494, 0x4c08, 0xd2a2, {0xc8, 0xec, 0x4f, 0xcf, 0xae, 0x8a}};
    }
    // offset 0, 8
    uint64_t magic0 = MAGIC0();
    UUID magic1 = MAGIC1();
    bool verify_magic() const {
        return magic0 == HeaderTrailer::MAGIC0() && magic1 == HeaderTrailer::MAGIC1();
    }

    // offset 24, 28
    uint32_t size = sizeof(HeaderTrailer);
    uint32_t flags = 0;

    static const uint32_t FLAG_SHIFT_HEADER = 0; // 1:header         0:trailer
    static const uint32_t FLAG_SHIFT_TYPE = 1;   // 1:data file,     0:index file
    static const uint32_t FLAG_SHIFT_SEALED = 2; // 1:YES,           0:NO
    static const uint32_t FLAG_SHIFT_GC = 3;     // 1:GC RO layer    0:Normal layer

    uint32_t get_flag_bit(uint32_t shift) const {
        return flags & (1 << shift);
    }
    void set_flag_bit(uint32_t shift) {
        flags |= (1 << shift);
    }
    void clr_flag_bit(uint32_t shift) {
        flags &= ~(1 << shift);
    }
    bool is_header() const {
        return get_flag_bit(FLAG_SHIFT_HEADER);
    }
    bool is_trailer() const {
        return !is_header();
    }
    bool is_data_file() const {
        return get_flag_bit(FLAG_SHIFT_TYPE);
    }
    bool is_index_file() const {
        return !is_data_file();
    }
    bool is_gc_file() const {
        return get_flag_bit(FLAG_SHIFT_GC);
    }
    bool is_sealed() const {
        return get_flag_bit(FLAG_SHIFT_SEALED);
    }
    void set_header() {
        set_flag_bit(FLAG_SHIFT_HEADER);
    }
    void set_trailer() {
        clr_flag_bit(FLAG_SHIFT_HEADER);
    }
    void set_data_file() {
        set_flag_bit(FLAG_SHIFT_TYPE);
    }
    void set_index_file() {
        clr_flag_bit(FLAG_SHIFT_TYPE);
    }
    void set_sealed() {
        set_flag_bit(FLAG_SHIFT_SEALED);
    }
    void clr_sealed() {
        clr_flag_bit(FLAG_SHIFT_SEALED);
    }
    void set_gc_file() {
        set_flag_bit(FLAG_SHIFT_GC);
    }
    void clr_gc_file() {
        set_flag_bit(FLAG_SHIFT_GC);
    }

    void set_uuid(const UUID uuid) {
        this->uuid = uuid;
    }

    int set_tag(char *buf, size_t n) {
        if (n > TAG_SIZE) {
            // auto tag_size = TAG_SIZE;  // work around for compiler err (gcc 4.9.2)..
            LOG_ERROR_RETURN(ENOBUFS, -1, "user tag too long. (need less than `)",
                             static_cast<uint32_t>(TAG_SIZE));
        }
        if (n == 0) {
            memset(user_tag, 0, sizeof(user_tag));
            return 0;
        }
        memcpy(user_tag, buf, n);
        return 0;
    }

    // offset 32, 40, 48
    uint64_t index_offset; // in bytes
    uint64_t index_size;   // # of SegmentMappings
    uint64_t virtual_size; // in bytes

    UUID::String uuid;        // 37 bytes.
    UUID::String parent_uuid; // 37 bytes.
    uint8_t from;             // DEPRECATED
    uint8_t to;               // DEPRECATED

    static const uint8_t LSMT_V1 = 1;     // v1 (UUID check)
    static const uint8_t LSMT_SUB_V1 = 1; // .1 deprecated level range.

    uint8_t version = LSMT_V1;
    uint8_t sub_version = LSMT_SUB_V1;

    char user_tag[TAG_SIZE]{}; // 256B commit message.

} __attribute__((packed));

static void usage() {
    static const char msg[] =
        "overlaybd-info [options] <data file> [index file]\n"
        "options:\n"
        "   -u only show UUID.\n"
        "   -r <registry_blob_url> read blob from registry.\n"
        "   -v show log detail.\n"
        "example:\n"
        "   ./overlaybd-info -u ./file.data ./file.index\n"
        "   ./overlaybd-info -u -r https://docker.io/v2/overlaybd/imgxxx/blobs/sha256:xxxxx\n";

    puts(msg);
    exit(0);
}

IFile *open(IFileSystem *fs, const char *fn, int flags, mode_t mode = 0) {
    auto file = fs->open(fn, flags, mode);
    if (!file) {
        fprintf(stderr, "failed to open file '%s', %d: %s\n", fn, errno, strerror(errno));
        exit(-1);
    }
    return file;
}

IFile *findex, *fdata;
int action = 0;
bool is_remote = false;
string url, cred_path;
IFileSystem *registryfs, *localfs;

static void parse_args(int &argc, char **argv) {
    int shift = 1;
    int ch;
    bool log = false;
    while ((ch = getopt(argc, argv, "vur:a:s:")) != -1) {
        switch (ch) {
        case 'u':
            action = 1;
            shift += 1;
            break;
        case 'r':
            is_remote = true;
            url = optarg;
            shift += 2;
            break;
        case 'v':
            log = true;
            log_output_level = 0;
            shift += 1;
            break;
        default:
            printf("invalid option: %c\n", char(ch));
            usage();
            exit(-1);
        }
    }
    if (!log) {
        log_output = log_output_null;
    }
    argc -= shift;
    if (!is_remote) {
        if (argc != 1 && argc != 2)
            return usage();

        localfs = new_localfs_adaptor();
        fdata = open(localfs, argv[shift++], O_RDONLY);

        if (argc == 2) {
            findex = open(localfs, argv[shift], O_RDONLY);
        }
        return;
    }
}

std::pair<std::string, std::string> reload_registry_auth(void *, const char *remote_path) {
    LOG_INFO("Acquire credential for ", VALUE(remote_path));
    std::string username, password;
    int res = load_cred_from_file(cred_path, std::string(remote_path), username, password);
    if (res == 0) {
        return std::make_pair(username, password);
    }
    printf("reload registry credential failed, token not found.\n");
    return std::make_pair("", "");
}

int main(int argc, char **argv) {
    parse_args(argc, argv);
    if (is_remote) {
        auto ret = photon::init();
        if (ret < 0) {
            printf("photon init failed.\n");
            return -1;
        }
        ret = photon::fd_events_init();
        if (ret < 0) {
            printf("photon fd_events_init failed.\n");
            return -1;
        }
        ret = Net::cURL::init();
        if (ret < 0) {
            printf("Net cURL init failed.\n");
            return -1;
        }

        auto p = url.find("sha256:");
        if (p == string::npos) {
            printf("invalid blob url.\n");
            exit(-1);
        }
        auto cafile = "/etc/ssl/certs/ca-bundle.crt";
        if (access(cafile, 0) != 0) {
            cafile = "/etc/ssl/certs/ca-certificates.crt";
            if (access(cafile, 0) != 0) {
                printf("no certificates found.");
                exit(-1);
            }
        }
        ImageConfigNS::GlobalConfig obd_conf;
        if (!obd_conf.ParseJSON("/etc/overlaybd/overlaybd.json")) {
            printf("invalid overlaybd config file.\n");
            exit(-1);
        }
        cred_path = obd_conf.credentialFilePath();

        auto prefix = url.substr(0, p);
        auto suburl = url.substr(p);
        LOG_INFO("create registryfs with cafile:`", cafile);
        auto registryfs = FileSystem::new_registryfs_with_credential_callback(
            {nullptr, &reload_registry_auth}, cafile, 30UL * 1000000);
        if (registryfs == nullptr) {
            printf("connect to registry failed.\n");
            exit(-1);
        }
        auto *f = reinterpret_cast<FileSystem::RegistryFile *>(registryfs->open(suburl.c_str(), 0));
        if (f == nullptr) {
            printf("open blob failed.\n");
            exit(-1);
        }
        fdata = f;
    }

    LSMT::IFile *fp = nullptr;
    LSMT::IFile *file = nullptr;
    HeaderTrailer *pht = nullptr;
    ALIGNED_MEM(buf, HeaderTrailer::SPACE, 4096);
    if (argc == 2) {
        file = open_file_rw(fdata, findex, false);
        auto ret = fdata->pread(buf, HeaderTrailer::SPACE, 0);
        if (ret != HeaderTrailer::SPACE) {
            delete file;
            fprintf(stderr, "failed to read lsmt file, possibly I/O error!\n");
            exit(-1);
        }
    } else {
        fdata = new_tar_file_adaptor(fdata);
        if (ZFile::is_zfile(fdata) == 1) {
            auto zfile = ZFile::zfile_open_ro(fdata, false);
            fp = zfile;
        } else {
            fp = fdata;
        }
        auto ret = fp->pread(buf, HeaderTrailer::SPACE, 0);
        if (ret != HeaderTrailer::SPACE) {
            delete fp;
            fprintf(stderr, "failed to read lsmt file, possibly I/O error!\n");
            exit(-1);
        }
        auto ht = (HeaderTrailer *)buf;
        if (ht->is_sealed()) {
            file = open_file_ro(fp);
            ret = fp->pread(buf, HeaderTrailer::SPACE, 0);
            if (ret != HeaderTrailer::SPACE) {
                delete file;
                fprintf(stderr, "failed to read lsmt file, possibly I/O error!\n");
                exit(-1);
            }
        } else {
            file = fp;
        }
    }

    pht = (HeaderTrailer *)buf;
    if (!file) {
        fprintf(stderr, "failed to create lsmt file object, possibly I/O error!\n");
        exit(-1);
    }

    if (action == 1) {
        printf("%s\n", (char *)&pht->uuid);
        // delete file;
        return 0;
    }
    if (pht->is_data_file()) {
        printf("Type: LSMT data file.\n");
    }
    if (pht->is_index_file()) {
        printf("Type: LSMT index file.\n");
    }
    if (pht->is_sealed()) {
        printf("Type: LSMT RO file.\n");
    }
    printf("Version: %u.%u\n", pht->version, pht->sub_version);
    printf("Virtual Size: %ld\n", file->lseek(0, SEEK_END));
    printf("User Tag: %s\n", pht->user_tag);
    if (UUID::String::is_valid((char *)&pht->uuid)) {
        printf("UUID: %s\n", (char *)&pht->uuid);
    } else {
        printf("UUID: null\n");
    }
    if (UUID::String::is_valid((char *)&pht->uuid)) {
        printf("Parent_UUID: %s\n", (char *)&pht->parent_uuid);
    } else {
        printf("Parent_UUID: null\n");
    }

    if (argc <= 1) {
        printf("Real Size: %lu\n", fdata->lseek(0, SEEK_END));
    } else {
        printf("Data Size: %ld\n", fdata->lseek(0, SEEK_END));
        printf("Index Size: %ld\n", findex->lseek(0, SEEK_END));
    }

    delete file;
    return 0;
}
