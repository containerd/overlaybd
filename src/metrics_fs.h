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

#include <photon/common/metric-meter/metrics.h>
#include <photon/fs/forwardfs.h>

struct MetricMeta {
    Metric::MaxLatencyCounter latency;
    Metric::QPSCounter throughput;
    Metric::QPSCounter qps;
    Metric::AddCounter total;
    Metric::AddCounter interval;

    MetricMeta() {}
};

class MetricFile : public photon::fs::ForwardFile_Ownership {
public:
    MetricMeta *metrics;

    MetricFile(photon::fs::IFile *file, MetricMeta *metricMeta)
        : photon::fs::ForwardFile_Ownership(file, true), metrics(metricMeta) {}

    __attribute__((always_inline)) void mark_metrics(ssize_t ret) {
        if (ret > 0) {
            metrics->throughput.put(ret);
            metrics->total.add(ret);
            metrics->interval.add(ret);
        }
    }

    virtual ssize_t pread(void *buf, size_t cnt, off_t offset) override {
        metrics->qps.put();
        SCOPE_LATENCY(metrics->latency);
        auto ret = m_file->pread(buf, cnt, offset);
        mark_metrics(ret);
        return ret;
    }

    virtual ssize_t preadv(const struct iovec *iovec, int iovcnt,
                           off_t offset) override {
        metrics->qps.put();
        SCOPE_LATENCY(metrics->latency);
        auto ret = m_file->preadv(iovec, iovcnt, offset);
        mark_metrics(ret);
        return ret;
    }

    virtual ssize_t preadv2(const struct iovec *iovec, int iovcnt, off_t offset,
                            int flags) override {
        metrics->qps.put();
        SCOPE_LATENCY(metrics->latency);
        auto ret = m_file->preadv2(iovec, iovcnt, offset, flags);
        mark_metrics(ret);
        return ret;
    }
};

class MetricFS : public photon::fs::ForwardFS_Ownership {
public:
    MetricMeta *metrics;

    MetricFS(photon::fs::IFileSystem *fs, MetricMeta *metricMeta)
        : photon::fs::ForwardFS_Ownership(fs, true), metrics(metricMeta) {}

    virtual photon::fs::IFile *open(const char *fn, int flags) override {
        auto file = m_fs->open(fn, flags);
        if (!file) return nullptr;
        return new MetricFile(file, metrics);
    }

    virtual photon::fs::IFile *open(const char *fn, int flags,
                                    mode_t mode) override {
        auto file = m_fs->open(fn, flags, mode);
        if (!file) return nullptr;
        return new MetricFile(file, metrics);
    }
};
