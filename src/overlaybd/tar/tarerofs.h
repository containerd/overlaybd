#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <tar.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/fiemap.h>
#include <photon/common/string_view.h>
#include <set>
#include <vector>
#include <string>
#include <list>
#include <map>

class TarErofs {
public:
    TarErofs(photon::fs::IFile *file, photon::fs::IFile *target, uint64_t fs_blocksize = 4096,
          photon::fs::IFile *bf = nullptr, bool meta_only = true, bool first_layer = true)
        : file(file), fout(target), fs_base_file(bf), meta_only(meta_only), first_layer(first_layer) {}

    int extract_all();

private:
    photon::fs::IFile *file = nullptr;     // source
    photon::fs::IFile *fout = nullptr; // target
    photon::fs::IFile *fs_base_file = nullptr;
    bool meta_only;
    bool first_layer;
    std::set<std::string> unpackedPaths;
    std::list<std::pair<std::string, int>> dirs; // <path, utime>
};
