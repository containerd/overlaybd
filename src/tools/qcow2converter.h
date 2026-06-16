#pragma once

#include <photon/fs/filesystem.h>
#include <cstdint>
// Default compression block size
constexpr uint32_t DEFAULT_BLOCK_SIZE = 4096;
// Write qcow2 clusters directly to an existing overlaybd IFile (ImageFile)
// Used by overlaybd-apply --from_qcow2: reads qcow2, pwrites to overlaybd layer
int convert_qcow2_to_imgfile(const char *input_path, photon::fs::IFile *target,
                              uint32_t block_size, bool verbose,
                              bool extract_rootfs);

