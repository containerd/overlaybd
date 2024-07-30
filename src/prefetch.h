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

#include <cctype>
#include <string>
#include <photon/fs/filesystem.h>

using namespace photon::fs;
/*
 * Prefetcher provides a capability which  can download remote data before the true I/O trigger.
 * It contains two modes:
 ==== static mode: replay the record I/O trace ====
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
 *
 ==== dynamic mode: reload specified the data from a external file list ====
 *  overlaybd will read the filelist to prefetch from the value of  'recordTracePath' in config.v1.json
 * {
 *    ...
 *    "recordTracePath": "path/to/filelist",
 *    ...
 *}
 * # cat /path/to/filelist
 * /absolute/path/to/fileA
 * /absolute/path/to/fileB
 * ...
 * /absolute/path/to/directory   ## support but not recommend
 *
 */
class Prefetcher : public Object {
public:
    enum class Mode {
        Disabled,
        Record,
        Replay,
    };
    enum class TraceOp : char { READ = 'R', WRITE = 'W' };

    virtual int record(TraceOp op, uint32_t layer_index, size_t count, off_t offset) = 0;

    virtual int replay(const IFile *imagefile = nullptr) = 0;

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

Prefetcher *new_prefetcher(const std::string &trace_file_path, int concurrency);
Prefetcher *new_dynamic_prefetcher(const std::string &prefetch_list, int concurrency);
