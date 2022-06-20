#pragma once

#include <photon/fs/filesystem.h>

extern "C" {
#include <ocf/ocf.h>
}

#define EASE_OCF_VOLUME_TYPE 1

struct ease_ocf_volume_params {
    size_t blk_size;
    size_t media_size;
    photon::fs::IFile *media_file;
    bool enable_logging;
};

int volume_init(ocf_ctx_t ocf_ctx);

void volume_cleanup(ocf_ctx_t ocf_ctx);
