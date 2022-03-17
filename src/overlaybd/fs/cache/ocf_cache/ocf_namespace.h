#pragma once

#include <unistd.h>

#include "../../../object.h"
#include "../../../estring.h"
#include "../../../callback.h"
#include "../../../fs/filesystem.h"

class OcfNamespace : public Object {
public:
    explicit OcfNamespace(size_t blk_size) : m_blk_size(blk_size) {
    }

    /**
     * @brief Validate parameters, and load some metadata into memory
     */
    virtual int init() = 0;

    /** NsInfo indicates a file's starting offset within its filesystem's address space, and its
     * size */
    struct NsInfo {
        off_t blk_idx;
        size_t file_size;
    };

    /**
     * @brief Locate a source file in namespace
     * @param[in] file_path
     * @param[in] src_file
     * @param[out] info
     * @retval 0 for success
     */
    virtual int locate_file(const estring &file_path, FileSystem::IFile *src_file,
                            NsInfo &info) = 0;

    size_t block_size() const {
        return m_blk_size;
    }

protected:
    size_t m_blk_size;
};

OcfNamespace *new_ocf_namespace_on_fs(size_t blk_size, FileSystem::IFileSystem *fs);

OcfNamespace *new_ocf_namespace_on_rocksdb(size_t blk_size);