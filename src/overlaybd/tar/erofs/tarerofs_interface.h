#ifndef TAREROFS_INTERFACE_H
#define TAREROFS_INTERFACE_H

#include <photon/fs/filesystem.h>
#include <photon/fs/fiemap.h>
#include <photon/common/string_view.h>

class TarErofsInter {
public:
    TarErofsInter(photon::fs::IFile *file, photon::fs::IFile *target, uint64_t fs_blocksize = 4096,
            photon::fs::IFile *bf = nullptr, bool meta_only = true, bool first_layer = true);
    ~TarErofsInter();

    int extract_all();

private:
    class TarErofsImpl;
    TarErofsImpl *impl;
};
#endif
