#include "libtar.h"

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/param.h>
#include <stdlib.h>
#include <dirent.h>
#include <string>
#include <set>
#include <photon/fs/path.h>
#include <photon/fs/filesystem.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>

// whiteoutPrefix prefix means file is a whiteout. If this is followed by a
// filename this means that file has been removed from the base layer.
// See https://github.com/opencontainers/image-spec/blob/main/layer.md#whiteouts
const std::string whiteoutPrefix = ".wh.";

// whiteoutMetaPrefix prefix means whiteout has a special meaning and is not
// for removing an actual file. Normally these files are excluded from exported
// archives.
const std::string whiteoutMetaPrefix = whiteoutPrefix + whiteoutPrefix;

// whiteoutOpaqueDir file means directory has been made opaque - meaning
// readdir calls to this directory do not follow to lower layers.
const std::string whiteoutOpaqueDir = whiteoutMetaPrefix + ".opq";

const std::string paxSchilyXattr = "SCHILY.xattr.";

int UnTar::mkdir_hier(const std::string_view &dir) {
    struct stat s;
    std::string path = remove_last_slash(dir);
    if (fs->lstat(path.c_str(), &s) == 0) {
        if (S_ISDIR(s.st_mode)) {
            return 0;
        } else {
            errno = ENOTDIR;
            // LOG_ERROR("mkdir ` failed, `", path.c_str(), strerror(errno));
            return -1;
        }
    }

    return photon::fs::mkdir_recursive(dir, fs, 0755);
}

int UnTar::remove_all(const std::string &path, bool rmdir) {
    if (fs == nullptr || path.empty()) {
        LOG_ERROR("remove_all ` failed, fs is null or path is empty", path);
        return -1;
    }
    struct stat statBuf;
    if (fs->lstat(path.c_str(), &statBuf) == 0) { // get stat
        if (S_ISDIR(statBuf.st_mode) == 0) {      // not dir
            if (unpackedPaths.find(path) == unpackedPaths.end()) {
                fs->unlink(path.c_str());
            }
            return 0;
        }
    } else {
        LOG_ERRNO_RETURN(0, -1, "get path ` stat failed", path);
    }

    auto dirs = fs->opendir(path.c_str());
    if (dirs == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "open dir ` failed", path);
    }
    dirent *dirInfo;
    while ((dirInfo = dirs->get()) != nullptr) {
        if (strcmp(dirInfo->d_name, ".") != 0 && strcmp(dirInfo->d_name, "..") != 0) {
            remove_all(path + "/" + dirInfo->d_name);
        }
        dirs->next();
    }

    fs->closedir(dirs);
    if (rmdir && unpackedPaths.find(path) == unpackedPaths.end()) {
        fs->rmdir(path.c_str());
    }

    return 0;
}

// 1 whiteout done
// 0 not whiteout
// -1 error
int UnTar::convert_whiteout(const char *filename) {
    photon::fs::Path p(filename);
    auto dir = std::string(p.dirname());
    auto base = p.basename();
    if (base == whiteoutOpaqueDir) {
        struct stat buf;
        std::string path = remove_last_slash(dir);
        if (fs->lstat(path.c_str(), &buf) < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to lstat ", VALUE(dir));
        }

        remove_all(path, false);
        return 1;
    }
    if (base.substr(0, whiteoutPrefix.size()) == whiteoutPrefix) {
        auto opath = dir + std::string(base.data() + whiteoutPrefix.size());
        remove_all(opath);
        return 1;
    }
    return 0;
}
