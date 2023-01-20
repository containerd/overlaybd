/*
Copyright 2022 The Overlaybd Authors

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
#include <cinttypes>
#include <map>
#include <photon/thread/thread.h>

class ThreadRangeLock {
public:
    int try_lock_wait(uint64_t offset, uint64_t length) {
        range_t r(offset, length);
        photon::scoped_lock lock(mtx);
        auto it = m_index.lower_bound(r);
        if (it != m_index.end() && it->first.offset < r.end()) {
            auto cond = it->second;
            cond->wait(lock);
            return -1;
        } else {
            it = m_index.emplace_hint(it, r, new photon::condition_variable);
            assert(it != m_index.end());
            return 0;
        }
    }

    void lock(uint64_t offset, uint64_t length) {
        while (true) {
            auto h = try_lock_wait(offset, length);
            if (h == 0)
                return;
        }
    }

    void unlock(uint64_t offset, uint64_t length) {
        range_t r(offset, length);
        photon::scoped_lock lock(mtx);
        auto it = m_index.find(r);
        assert(it != m_index.end());
        it->second->notify_all();
        delete it->second;
        m_index.erase(it);
    }

protected:
    photon::mutex mtx;
    struct range_t {
        uint64_t offset;
        uint32_t length;
        range_t() {
        }
        range_t(uint64_t offset, uint64_t length) {
            this->offset = offset;
            this->length = length;
        }
        uint64_t end() const {
            return offset + length;
        }
        bool operator<(const range_t &rhs) const {
            return end() <= rhs.offset; // because end() is not inclusive
        }
        bool contains(const range_t &x) const {
            return offset <= x.offset && end() >= x.end();
        }
    } __attribute__((packed));
    std::map<range_t, photon::condition_variable *> m_index;
};

class ThreadScopedRangeLock {
public:
    ThreadScopedRangeLock(ThreadRangeLock &lock, uint64_t offset, uint64_t length)
        : _lock(&lock), _offset(offset), _length(length) {
        _lock->lock(_offset, _length);
    }
    ~ThreadScopedRangeLock() {
        _lock->unlock(_offset, _length);
    }

protected:
    ThreadRangeLock *_lock;
    uint64_t _offset, _length;
};
