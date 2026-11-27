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

#pragma once

#include <photon/fs/virtual-file.h>
#include <vector>

class FsMetaFile : public photon::fs::VirtualFile {
public:
    static constexpr size_t BLOCK_SIZE = 4096;

    FsMetaFile(photon::fs::IFile *base, photon::fs::IFile *sparse, size_t size)
        : m_base(base), m_sparse(sparse), m_size(size),
          m_dirty((size + BLOCK_SIZE * 64 - 1) / (BLOCK_SIZE * 64)) {}

    ssize_t pread(void *buf, size_t count, off_t offset) override;
    ssize_t pwrite(const void *buf, size_t count, off_t offset) override;
    int fallocate(int mode, off_t offset, off_t len) override;
    int fstat(struct stat *st) override { return m_base->fstat(st); }
    int fsync() override { return m_sparse->fsync(); }
    int fdatasync() override { return m_sparse->fdatasync(); }
    int close() override { return 0; }
    photon::fs::IFileSystem *filesystem() override { return m_base->filesystem(); }

    void discard(off_t offset, size_t count);
    int commit();

    UNIMPLEMENTED(int fchmod(mode_t mode) override);
    UNIMPLEMENTED(int fchown(uid_t owner, gid_t group) override);
    UNIMPLEMENTED(int ftruncate(off_t length) override);

private:
    photon::fs::IFile *m_base;
    photon::fs::IFile *m_sparse;
    size_t m_size;
    std::vector<uint64_t> m_dirty;
    int m_write_error = 0;

    bool dirty(size_t block) const;
    void mark(size_t block);
    void clear(size_t block);
    bool valid_range(off_t offset, size_t count) const;
};
