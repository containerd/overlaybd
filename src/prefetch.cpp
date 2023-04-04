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
#include <memory>
#include <vector>
#include <map>
#include <queue>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "prefetch.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/forwardfs.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread11.h>
#include "overlaybd/zfile/crc32/crc32c.h"

using namespace std;

class PrefetcherImpl;

class PrefetchFile : public ForwardFile_Ownership {
public:
    PrefetchFile(IFile *src_file, uint32_t layer_index, Prefetcher *prefetcher);

    ssize_t pread(void *buf, size_t count, off_t offset) override;

private:
    uint32_t m_layer_index;
    PrefetcherImpl *m_prefetcher;
};

class PrefetcherImpl : public Prefetcher {
public:
    explicit PrefetcherImpl(const string &trace_file_path, int concurrency) : m_concurrency(concurrency) {
        // Detect mode
        size_t file_size = 0;
        m_mode = detect_mode(trace_file_path, &file_size);
        m_lock_file_path = trace_file_path + ".lock";
        m_ok_file_path = trace_file_path + ".ok";
        LOG_INFO("Prefetch: run with mode `, trace file is `", m_mode, trace_file_path);

        // Open trace file
        if (m_mode != Mode::Disabled) {
            int flags = m_mode == Mode::Record ? O_WRONLY : O_RDONLY;
            m_trace_file =
                open_localfile_adaptor(trace_file_path.c_str(), flags, 0666);
        }

        // Loop detect lock file if going to record
        if (m_mode == Mode::Record) {
            int lock_fd = open(m_lock_file_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_EXCL, 0666);
            close(lock_fd);
            auto th = photon::thread_create11(&PrefetcherImpl::detect_lock, this);
            m_detect_thread = photon::thread_enable_join(th);
        }

        // Reload if going to replay
        if (m_mode == Mode::Replay) {
            reload(file_size);
        }
    }

    ~PrefetcherImpl() {
        if (m_mode == Mode::Record) {
            m_record_stopped = true;
            if (m_detect_thread_interruptible) {
                photon::thread_shutdown((photon::thread *)m_detect_thread);
            }
            photon::thread_join(m_detect_thread);
            dump();

        } else if (m_mode == Mode::Replay) {
            m_replay_stopped = true;
            if (m_replay_thread) {
                for (auto th : m_replay_threads) {
                    if (th) {
                        photon::thread_shutdown((photon::thread *)th);
                    }
                }
                photon::thread_join(m_replay_thread);
            }
        }

        if (m_trace_file != nullptr) {
            safe_delete(m_trace_file);
        }
    }

    IFile *new_prefetch_file(IFile *src_file, uint32_t layer_index) override {
        return new PrefetchFile(src_file, layer_index, this);
    }

    void record(TraceOp op, uint32_t layer_index, size_t count, off_t offset) override {
        if (m_record_stopped) {
            return;
        }
        TraceFormat trace = {op, layer_index, count, offset};
        m_record_array.push_back(trace);
    }

    void do_replay() {
        struct timeval start;
        gettimeofday(&start, NULL);
        LOG_INFO("Prefetch: Replay ` records from ` layers, concurrency `",
                 m_replay_queue.size(), m_src_files.size(), m_concurrency);
        for (int i = 0; i < m_concurrency; ++i) {
            auto th = photon::thread_create11(&PrefetcherImpl::replay_worker_thread, this);
            auto join_handle = photon::thread_enable_join(th);
            m_replay_threads.push_back(join_handle);
        }
        for (auto &th : m_replay_threads) {
            photon::thread_join(th);
            th = nullptr;
        }
        struct timeval end;
        gettimeofday(&end, NULL);
        uint64_t elapsed = 1000000UL * (end.tv_sec - start.tv_sec) + end.tv_usec - start.tv_usec;
        LOG_INFO("Prefetch: Replay done, time cost ` ms", elapsed / 1000);
    }

    void replay() override {
        if (m_mode != Mode::Replay) {
            return;
        }
        if (m_replay_queue.empty() || m_src_files.empty()) {
            return;
        }
        auto th = photon::thread_create11(&PrefetcherImpl::do_replay, this);
        m_replay_thread = photon::thread_enable_join(th);
    }

    int replay_worker_thread() {
        while (!m_replay_queue.empty() && !m_replay_stopped) {
            auto trace = m_replay_queue.front();
            m_replay_queue.pop();
            auto iter = m_src_files.find(trace.layer_index);
            if (iter == m_src_files.end()) {
                continue;
            }
            auto src_file = iter->second;
            if (trace.op == PrefetcherImpl::TraceOp::READ) {
                ssize_t n_read = src_file->pread(nullptr, trace.count, trace.offset);
                if (n_read != (ssize_t)trace.count) {
                    LOG_WARN("Prefetch: replay pread failed: `, `, respect: `, got: `", ERRNO(),
                              trace, trace.count, n_read);
                    continue;
                }
            }
        }
        return 0;
    }

    void register_src_file(uint32_t layer_index, IFile *src_file) {
        m_src_files[layer_index] = src_file;
    }

private:
    struct TraceFormat {
        TraceOp op;
        uint32_t layer_index;
        size_t count;
        off_t offset;
    };

    struct TraceHeader {
        uint32_t magic = 0;
        size_t data_size = 0;
        uint32_t checksum = 0;
    };

    static const int MAX_IO_SIZE = 1024 * 1024;
    static const uint32_t TRACE_MAGIC = 3270449184; // CRC32 of `Container Image Trace Format`

    vector<TraceFormat> m_record_array;
    queue<TraceFormat> m_replay_queue;
    map<uint32_t, IFile *> m_src_files;
    vector<photon::join_handle *> m_replay_threads;
    photon::join_handle *m_replay_thread = nullptr;
    photon::join_handle *m_detect_thread = nullptr;
    bool m_detect_thread_interruptible = false;
    string m_lock_file_path;
    string m_ok_file_path;
    IFile *m_trace_file = nullptr;
    bool m_replay_stopped = false;
    bool m_record_stopped = false;
    bool m_buffer_released = false;
    int m_concurrency;

    int dump() {
        if (m_trace_file == nullptr) {
            return 0;
        }

        if (access(m_ok_file_path.c_str(), F_OK) != 0) {
            unlink(m_ok_file_path.c_str());
        }

        auto close_trace_file = [&]() {
            if (m_trace_file != nullptr) {
                m_trace_file->close();
                m_trace_file = nullptr;
            }
        };
        DEFER(close_trace_file());

        TraceHeader hdr = {};
        hdr.magic = TRACE_MAGIC;
        hdr.checksum = 0; // calculate and re-write checksum later
        hdr.data_size = sizeof(TraceFormat) * m_record_array.size();

        ssize_t n_written = m_trace_file->write(&hdr, sizeof(TraceHeader));
        if (n_written != sizeof(TraceHeader)) {
            m_trace_file->ftruncate(0);
            LOG_ERRNO_RETURN(0, -1, "Prefetch: dump write header failed");
        }

        for (auto &each : m_record_array) {
            hdr.checksum = crc32::crc32c_extend(&each, sizeof(TraceFormat), hdr.checksum);
            n_written = m_trace_file->write(&each, sizeof(TraceFormat));
            if (n_written != sizeof(TraceFormat)) {
                m_trace_file->ftruncate(0);
                LOG_ERRNO_RETURN(0, -1, "Prefetch: dump write content failed");
            }
        }

        n_written = m_trace_file->pwrite(&hdr, sizeof(TraceHeader), 0);
        if (n_written != sizeof(TraceHeader)) {
            m_trace_file->ftruncate(0);
            LOG_ERRNO_RETURN(0, -1, "Prefetch: dump write header(checksum) failed");
        }

        unlink(m_lock_file_path.c_str());

        int ok_fd = open(m_ok_file_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_EXCL, 0666);
        if (ok_fd < 0) {
            LOG_ERRNO_RETURN(0, -1, "Prefetch: open OK file failed");
        }
        close(ok_fd);
        LOG_INFO("Prefetch: Record ` records", m_record_array.size());
        return 0;
    }

    int reload(size_t trace_file_size) {
        // Reload header
        TraceHeader hdr = {};
        ssize_t n_read = m_trace_file->read(&hdr, sizeof(TraceHeader));
        if (n_read != sizeof(TraceHeader)) {
            LOG_ERRNO_RETURN(0, -1, "Prefetch: reload header failed");
        }
        if (TRACE_MAGIC != hdr.magic) {
            LOG_ERROR_RETURN(0, -1, "Prefetch: trace magic mismatch");
        }
        if (trace_file_size != hdr.data_size + sizeof(TraceHeader)) {
            LOG_ERROR_RETURN(0, -1, "Prefetch: trace file size mismatch");
        }

        // Reload content
        uint32_t checksum = 0;
        TraceFormat fmt = {};
        for (int i = 0; i < hdr.data_size / sizeof(TraceFormat); ++i) {
            n_read = m_trace_file->read(&fmt, sizeof(TraceFormat));
            if (n_read != sizeof(TraceFormat)) {
                LOG_ERRNO_RETURN(0, -1, "Prefetch: reload content failed");
            }
            checksum = crc32::crc32c_extend(&fmt, sizeof(TraceFormat), checksum);
            // Save in memory
            m_replay_queue.push(fmt);
        }

        if (checksum != hdr.checksum) {
            queue<TraceFormat> tmp;
            m_replay_queue.swap(tmp);
            LOG_ERROR_RETURN(0, -1, "Prefetch: reload checksum error");
        }

        LOG_INFO("Prefetch: Reload ` records", m_replay_queue.size());
        return 0;
    }

    int detect_lock() {
        while (!m_record_stopped) {
            m_detect_thread_interruptible = true;
            int ret = photon::thread_sleep(1);
            m_detect_thread_interruptible = false;
            if (ret != 0) {
                break;
            }
            if (access(m_lock_file_path.c_str(), F_OK) != 0) {
                m_record_stopped = true;
                dump();
                break;
            }
        }
        return 0;
    }

    friend LogBuffer &operator<<(LogBuffer &log, const PrefetcherImpl::TraceFormat &f);
};

LogBuffer &operator<<(LogBuffer &log, const PrefetcherImpl::TraceFormat &f) {
    return log << "Op " << char(f.op) << ", Count " << f.count << ", Offset " << f.offset
               << ", Layer_index " << f.layer_index;
}

PrefetchFile::PrefetchFile(IFile *src_file, uint32_t layer_index, Prefetcher *prefetcher)
    : ForwardFile_Ownership(src_file, true), m_layer_index(layer_index),
      m_prefetcher((PrefetcherImpl *)prefetcher) {
    if (m_prefetcher->get_mode() == PrefetcherImpl::Mode::Replay) {
        m_prefetcher->register_src_file(layer_index, src_file);
    }
}

ssize_t PrefetchFile::pread(void *buf, size_t count, off_t offset) {
    ssize_t n_read = m_file->pread(buf, count, offset);
    if (n_read == (ssize_t)count && m_prefetcher->get_mode() == PrefetcherImpl::Mode::Record) {
        m_prefetcher->record(PrefetcherImpl::TraceOp::READ, m_layer_index, count, offset);
    }
    return n_read;
}

Prefetcher *new_prefetcher(const string &trace_file_path, int concurrency) {
    return new PrefetcherImpl(trace_file_path, concurrency);
}

Prefetcher::Mode Prefetcher::detect_mode(const string &trace_file_path, size_t *file_size) {
    struct stat buf = {};
    int ret = stat(trace_file_path.c_str(), &buf);
    if (file_size != nullptr) {
        *file_size = buf.st_size;
    }
    if (ret != 0) {
        return Mode::Disabled;
    } else if (buf.st_size == 0) {
        return Mode::Record;
    } else {
        return Mode::Replay;
    }
}
