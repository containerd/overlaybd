/*
 * prefetch.h
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#pragma once

#include <cctype>
#include <string>
#include <photon/fs/filesystem.h>

using namespace photon::fs;
/*
 * 1. Prefetcher supports `record` and `replay` operations on a specific container image.
 *    It will persist R/W metadata of all layers into a trace file during container boot-up,
 *    and then replay this file, in order to retrieve data in advance.
 *
 * 2. When starting recording, a lock file will be created. Delete it to stop recording.
 *
 * 3. After recording stopped, data will be dumped into trace file. A OK file will be created
 *    to indicate dump finished.
 *
 * 4. In conclusion, the work modes are as follows:
 *      trace file non-existent                         => Disabled
 *      trace file exist but empty                      => Start Recording
 *      lock file deleted or prefetcher destructed      => Stop Recording
 *      trace file exist and not empty                  => Replay
 */
class Prefetcher : public Object {
public:
    enum class Mode {
        Disabled,
        Record,
        Replay,
    };
    enum class TraceOp : char { READ = 'R', WRITE = 'W' };

    virtual void record(TraceOp op, uint32_t layer_index, size_t count, off_t offset) = 0;

    virtual void replay() = 0;

    // Prefetch file inherits ForwardFile, and it is the actual caller of `record` method.
    // The source file is supposed to have cache.
    virtual IFile *new_prefetch_file(IFile *src_file, uint32_t layer_index) = 0;

    static Mode detect_mode(const std::string &trace_file_path, size_t *file_size = nullptr);

    Mode get_mode() const {
        return m_mode;
    }

protected:
    Mode m_mode;
};

Prefetcher *new_prefetcher(const std::string &trace_file_path);
