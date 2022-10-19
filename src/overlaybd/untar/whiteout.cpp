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
#include <photon/common/string_view.h>
#include <photon/fs/filesystem.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/enumerable.h>


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


int remove_all(photon::fs::IFileSystem *fs, const std::string &path) {
    if (fs == nullptr || path.empty()) {
        LOG_ERROR("remove_all ` failed, fs is null or path is empty", path);
        return -1;
    }
    struct stat statBuf;
    if (fs->lstat(path.c_str(), &statBuf) == 0) {        // get stat
        if (S_ISDIR(statBuf.st_mode) == 0) {      // not dir
            fs->unlink(path.c_str());
            return 0;
        }
    } else {
        LOG_DEBUG("get path ` stat failed, errno `:`", path, errno, strerror(errno));
        return -1;
    }

    auto dirs = fs->opendir(path.c_str());
    if (dirs == nullptr) {
        LOG_ERROR("open dir ` failed, errno `:`", path, errno, strerror(errno));
        return -1;
    }
    dirent *dirInfo;
    while ((dirInfo = dirs->get()) != nullptr) {
        if (strcmp(dirInfo->d_name, ".") != 0 && strcmp(dirInfo->d_name, "..") != 0) {
            remove_all(fs, path + "/" + dirInfo->d_name);
        }
        dirs->next();
    }

    fs->closedir(dirs);
    fs->rmdir(path.c_str());

    return 0;
}

// 1 whiteout done
// 0 not whiteout
// -1 error
int Tar::convert_whiteout(const char *filename) {
	photon::fs::Path p(filename);
	auto dir = std::string(p.dirname());
	auto base = p.basename();
	if (base == whiteoutOpaqueDir) {
		struct stat buf;
		if (fs->lstat(dir.c_str(), &buf) < 0) {
			LOG_ERRNO_RETURN(0, -1, "failed to lstat ", VALUE(dir));
		}

		for (auto file : enumerable(photon::fs::Walker(fs, dir))) {
			auto fn = std::string(file);
			if (unpackedPaths.find(fn) == unpackedPaths.end()) {
				remove_all(fs, fn);
			}
  		}
		return 1;
	}
	if (base.substr(0, whiteoutPrefix.size()) == whiteoutPrefix) {
		auto opath = dir + std::string(base.data() + whiteoutPrefix.size());
		remove_all(fs, opath);
		return 1;
	}
	return 0;
}

