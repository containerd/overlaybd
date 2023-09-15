
#include <photon/fs/localfs.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/virtual-file.h>
#include <string>

class SHA256File : public photon::fs::VirtualReadOnlyFile {
public:
    virtual std::string sha256_checksum() = 0;
};

SHA256File *new_sha256_file(photon::fs::IFile *file, bool ownership);

std::string sha256sum(const char *fn);
