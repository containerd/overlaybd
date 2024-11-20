#ifndef TAREROFS_INTERFACE_H
#define TAREROFS_INTERFACE_H

#include <photon/fs/filesystem.h>
#include <photon/fs/fiemap.h>
#include <photon/common/string_view.h>

class LibErofs {
public:
    LibErofs(photon::fs::IFile *target, uint64_t blksize, bool import_tar_headers = false);
    ~LibErofs();
    int extract_tar(photon::fs::IFile *source, bool meta_only, bool first_layer);
private:
    photon::fs::IFile * target= nullptr; /* output file */
    uint64_t blksize;
    bool ddtaridx;
};

#endif
