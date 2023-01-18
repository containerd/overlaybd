#pragma once

#include "metrics.h"
#include <photon/common/alog.h>
#include <photon/fs/forwardfs.h>

#include <vector>

class MetricMeta {
public:
    // TODO: add more metrics (error count, virtual block device IO latency, etc...)
    Metric::AverageLatencyCounter latency;
};

class MetricFile : public photon::fs::ForwardFile_Ownership {
public:
    MetricMeta *metrics;

    MetricFile(IFile *file, MetricMeta *metricMeta)
        : photon::fs::ForwardFile_Ownership(file, true), metrics(metricMeta) {}

    virtual ssize_t pread(void *buf, size_t cnt,
                           off_t offset) override {
        auto start = photon::now;
        auto ret = m_file->pread(buf, cnt, offset);
        if (ret) {
            auto duration = photon::now - start;
            // latency of read 1MB
            metrics->latency.put(duration<<20, ret);
        }
        return ret;
    }

    virtual ssize_t preadv(const struct iovec *iovec, int iovcnt,
                           off_t offset) override {
        auto start = photon::now;
        auto ret = m_file->preadv(iovec, iovcnt, offset);
        if (ret) {
            auto duration = photon::now - start;
            metrics->latency.put(duration<<20, ret);
        }
        return ret;
    }

    virtual ssize_t preadv2(const struct iovec *iovec, int iovcnt, off_t offset,
                            int flags) override {
        auto start = photon::now;
        auto ret = m_file->preadv2(iovec, iovcnt, offset, flags);
        if (ret) {
            auto duration = photon::now - start;
            metrics->latency.put(duration<<20, ret);
        }
        return ret;
    }
};

class MetricFS : public photon::fs::ForwardFS_Ownership {
public:
    MetricMeta *metrics;
    MetricFS(IFileSystem *fs, MetricMeta *metricMeta)
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
