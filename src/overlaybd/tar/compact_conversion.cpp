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

#include "compact_conversion.h"
#include "fsmeta_file.h"
#include "libtar.h"
#include "../lsmt/file.h"
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
#include <photon/common/alog.h>
#include <photon/fs/aligned-file.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/fs/localfs.h>

namespace CompactConversion {
namespace {

using photon::fs::IFile;

LSMT::IFileRW *append_target(IFile *image, IFile *base) {
    if (image == nullptr) {
        LOG_ERROR_RETURN(EINVAL, nullptr, "compact conversion requires an image");
    }
    auto target = dynamic_cast<LSMT::IFileRW *>(base ? base : image);
    if (target == nullptr || target->can_map_staged_data() != 0) {
        LOG_ERROR_RETURN(ENOTSUP, nullptr, "compact conversion requires a plain Append layer");
    }
    return target;
}

std::unique_ptr<IFile> create_scratch() {
    auto dir = getenv("TMPDIR");
    std::string path = std::string(dir && *dir ? dir : "/tmp") + "/overlaybd-XXXXXX";
    int fd = mkstemp(&path[0]);
    if (fd < 0) {
        LOG_ERRNO_RETURN(0, nullptr, "failed to create temporary file `", path.c_str());
    }
    if (unlink(path.c_str()) != 0) {
        auto error = errno;
        close(fd);
        LOG_ERRNO_RETURN(error, nullptr, "failed to unlink temporary file `", path.c_str());
    }
    std::unique_ptr<IFile> file(photon::fs::new_localfile_adaptor(fd));
    if (!file) {
        auto error = errno;
        close(fd);
        errno = error;
    }
    return file;
}

// Declaration order keeps the metadata view alive until ext4 has been closed,
// and destroys it before either of its backing views.
struct MetadataStage {
    std::unique_ptr<IFile> aligned;
    std::unique_ptr<IFile> scratch;
    std::unique_ptr<FsMetaFile> file;

    int open(IFile *image) {
        struct stat st;
        if (image->fstat(&st) != 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to stat compact conversion image");
        }
        if (st.st_size < 0) {
            LOG_ERROR_RETURN(EINVAL, -1, "invalid compact conversion image size");
        }
        aligned.reset(photon::fs::new_aligned_file_adaptor(image, FsMetaFile::BLOCK_SIZE,
                                                         true, false));
        if (!aligned) {
            LOG_ERRNO_RETURN(0, -1, "failed to create compact conversion alignment adaptor");
        }
        scratch = create_scratch();
        if (!scratch) {
            return -1;
        }
        file.reset(new FsMetaFile(aligned.get(), scratch.get(), st.st_size));
        return 0;
    }
};

} // namespace

int mkfs(IFile *image, IFile *base) {
    if (append_target(image, base) == nullptr) {
        return -1;
    }
    MetadataStage metadata;
    if (metadata.open(image) != 0) {
        return -1;
    }
    // make_extfs closes its ext4 state before returning; check staged I/O errors
    // and commit only after those final metadata writes have completed.
    if (photon::fs::make_extfs(metadata.file.get()) != 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to format compact conversion image");
    }
    return metadata.file->commit();
}

int apply(IFile *source, IFile *image, IFile *base, bool mkfs,
          const std::function<int()> &finalize_source) {
    if (source == nullptr) {
        LOG_ERROR_RETURN(EINVAL, -1, "compact conversion requires a tar source");
    }
    auto target = append_target(image, base);
    if (target == nullptr) {
        return -1;
    }
    auto files = target->get_lower_files();
    if (files.empty() || files.back() == nullptr) {
        LOG_ERROR_RETURN(EINVAL, -1, "compact conversion has no upper data file");
    }
    auto headers = create_scratch();
    if (!headers) {
        return -1;
    }
    TarCore scan(source, 0, FsMetaFile::BLOCK_SIZE);
    if (scan.dump_tar_headers(headers.get(), files.back()) < 0) {
        return -1;
    }
    if (finalize_source && finalize_source() != 0) {
        return -1;
    }
    if (headers->lseek(0, SEEK_SET) != 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to rewind compact conversion headers");
    }

    MetadataStage metadata;
    if (metadata.open(image) != 0) {
        return -1;
    }
    if (mkfs && photon::fs::make_extfs(metadata.file.get()) != 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to format compact conversion image");
    }
    // ext4 must be destroyed before metadata, including on every error path.
    std::unique_ptr<photon::fs::IFileSystem> fs(
        photon::fs::new_extfs(metadata.file.get(), false));
    if (!fs) {
        LOG_ERRNO_RETURN(0, -1, "failed to open compact conversion ext4 filesystem");
    }
    UnTar replay(headers.get(), fs.get(), 0, FsMetaFile::BLOCK_SIZE, target);
    return replay.extract_staged(metadata.file.get(), [&fs]() { fs.reset(); });
}

} // namespace CompactConversion
