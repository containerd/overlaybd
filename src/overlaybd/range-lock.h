/*
 * range-lock.h
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
#include <inttypes.h>
#include <set>
#include "photon/thread.h"

class RangeLock {
public:
    // return 0 if successfully locked;
    // otherwise, wait and return -1 and the conflicted range
    int try_lock_wait(uint64_t &offset, uint64_t &length) {
        range_t r(offset, length);
        auto it = m_index.lower_bound(r);
        if (it != m_index.end() && it->offset < r.end()) {
            offset = it->offset * ALIGNMENT;
            length = std::min(it->end(), r.end()) * ALIGNMENT - offset;
            it->cond.wait_no_lock();
            return -1;
        } else {
            m_index.emplace_hint(it, r);
            return 0;
        }
    }

    void unlock(uint64_t offset, uint64_t length) {
        range_t r(offset, length);
        auto it = m_index.lower_bound(r);
        while (it != m_index.end() && it->offset < r.end()) {
            if (r.contains(*it)) {
                it = m_index.erase(it);
            } else {
                ++it;
            }
        }
    }

    struct LockHandle;

    LockHandle *try_lock_wait2(uint64_t offset, uint64_t length) {
        range_t r(offset, length);
        auto it = m_index.lower_bound(r);
        if (it != m_index.end() && it->offset < r.end()) {
            it->cond.wait_no_lock();
            return nullptr;
        } else {
            it = m_index.emplace_hint(it, r);
            assert(it != m_index.end());
            static_assert(sizeof(it) == sizeof(LockHandle *), "...");
            return (LockHandle *&)it;
        }
    }

    LockHandle *lock(uint64_t offset, uint64_t length) {
        while (true) {
            auto h = try_lock_wait2(offset, length);
            if (h)
                return h;
        }
    }

    int adjust_range(LockHandle *h, uint64_t offset, uint64_t length) {
        if (!h)
            return -1;
        range_t r1(offset, length);
        auto it = (iterator &)h;
        auto r0 = (range_t *)&*it;
        if ((r1.offset < r0->offset && r1.offset < prev_end(it)) ||
            (r1.end() > it->end() && r1.end() > next_offset(it)))
            return -1;
        r0->offset = r1.offset;
        r0->length = r1.length;
        return 0;
    }

    void unlock(LockHandle *h) {
        auto it = (iterator &)h;
        m_index.erase(it);
    }

protected:
    struct range_t {
        uint64_t offset : 50; // offset (0.5 PB if in sector)
        uint32_t length : 14; // length (8MB if in sector)
        range_t() {
        }
        range_t(uint64_t offset, uint64_t length) {
            align(offset, length);
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
    struct Range : public range_t {
        using range_t::range_t;
        mutable photon::condition_variable cond;
        Range(const Range &rhs) : range_t(rhs) {
        }
        Range(const range_t &rhs) : range_t(rhs) {
        }
        ~Range() {
            cond.notify_all();
        }
    };
    std::set<Range> m_index;
    typedef std::set<Range>::iterator iterator;
    uint64_t next_offset(iterator it) {
        return (++it == m_index.end()) ? (uint64_t)-1 : it->offset;
    }
    uint64_t prev_end(iterator it) {
        return (it == m_index.begin()) ? 0 : (--it)->end();
    }
    const static uint64_t ALIGNMENT = 512;
    static void align(uint64_t &x) // align down x
    {
        x /= ALIGNMENT;
    }
    static void align(uint64_t &offset, uint64_t &length) { // align down offset, and up length
        auto end = offset + length + ALIGNMENT - 1;
        align(offset);
        align(end);
        length = end - offset;
    }
};

class ScopedRangeLock {
public:
    ScopedRangeLock(RangeLock &lock, uint64_t offset, uint64_t length) : _lock(&lock) {
        _h = _lock->lock(offset, length);
    }
    ~ScopedRangeLock() {
        _lock->unlock(_h);
    }

protected:
    RangeLock *_lock;
    RangeLock::LockHandle *_h;
};
