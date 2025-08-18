#include <fcntl.h>
#include <openssl/sha.h>
#include "sha256file.h"
#include <photon/common/alog.h>
#include <string>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

using namespace photon::fs;
using namespace std;

class SHA256CheckedFile: public SHA256File {
public:
    IFile *m_file;
    SHA256_CTX ctx = {0};
    size_t total_read = 0;
    bool m_ownership = false;

    SHA256CheckedFile(IFile *file, bool ownership): m_file(file), m_ownership(ownership) {
        SHA256_Init(&ctx);
    }
    ~SHA256CheckedFile() {
        if (m_ownership) delete m_file;
    }
    virtual IFileSystem *filesystem() override {
        return nullptr;
    }
    ssize_t read(void *buf, size_t count) override {
        auto rc = m_file->read(buf, count);
        if (rc > 0 && SHA256_Update(&ctx, buf, rc) < 0) {
            LOG_ERROR("sha256 calculate error");
            return -1;
        }
        return rc;
    }
    off_t lseek(off_t offset, int whence) override {
        return m_file->lseek(offset, whence);
    }
    virtual std::string sha256_checksum() override{
        // read trailing data
        char buf[64*1024];
        auto rc = m_file->read(buf, 64*1024);
        while (rc > 0) {
        // if (rc == 64*1024) {
        //     LOG_WARN("too much trailing data");
        // }
            if (rc > 0 && SHA256_Update(&ctx, buf, rc) < 0) {
                LOG_ERROR("sha256 calculate error");
                return "";
            }
            rc = m_file->read(buf, 64*1024);
        }
        // calc sha256 result
        unsigned char sha[32];
        SHA256_Final(sha, &ctx);
        char res[SHA256_DIGEST_LENGTH * 2];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            sprintf(res + (i * 2), "%02x", sha[i]);
        return "sha256:" + std::string(res, SHA256_DIGEST_LENGTH * 2);
    }
    int fstat(struct stat *buf) override {
        return m_file->fstat(buf);
    }
};

SHA256File *new_sha256_file(IFile *file, bool ownership = true) {
    return new SHA256CheckedFile(file, ownership);
}

string sha256sum(const char *fn) {
    constexpr size_t BUFFERSIZE = 65536;
    // auto file = open_localfile_adaptor(fn, O_RDONLY | O_DIRECT);
    // auto sha256file = new_sha256_file(file, true);
    // DEFER(delete sha256file);
    // return sha256file->sha256_checksum();
    int fd = open(fn, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        LOG_ERROR("failed to open `", fn);
        return "";
    }
    DEFER(close(fd););

    struct stat stat;
    if (::fstat(fd, &stat) < 0) {
        LOG_ERROR("failed to stat `", fn);
        return "";
    }
    SHA256_CTX ctx = {0};
    SHA256_Init(&ctx);
    __attribute__((aligned(ALIGNMENT_4K))) char buffer[65536];
    unsigned char sha[32];
    ssize_t recv = 0;
    for (off_t offset = 0; offset < stat.st_size; offset += BUFFERSIZE) {
        recv = pread(fd, &buffer, BUFFERSIZE, offset);
        if (recv < 0) {
            LOG_ERROR("io error: `", fn);
            return "";
        }
        if (SHA256_Update(&ctx, buffer, recv) < 0) {
            LOG_ERROR("sha256 calculate error: `", fn);
            return "";
        }
    }
    SHA256_Final(sha, &ctx);
    char res[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(res + (i * 2), "%02x", sha[i]);
    return "sha256:" + std::string(res, SHA256_DIGEST_LENGTH * 2);
}
