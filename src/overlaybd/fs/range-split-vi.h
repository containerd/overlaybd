/*
  * range-split-vi.h
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
#include <assert.h>
#include <algorithm>
#include "range-split.h"

namespace FileSystem
{
    // variable interval
    struct range_split_vi : public basic_range_split<range_split_vi>
    {
        const uint64_t* key_points;
        uint64_t n;

        // the `key_points` are composed of `n` (at least 3) ascending points (offsets),
        // begining with 0, and ending with UINT64_MAX !!
        range_split_vi(uint64_t offset, uint64_t length, const uint64_t* key_points, uint64_t n) :
            key_points(key_points), n(n)
        {
            assert(n >= 3);
            assert(key_points[0] == 0);
            assert(key_points[n-1] == UINT64_MAX);
            assert(ascending(key_points, n));
            init(offset, length);
        }
        bool ascending(const uint64_t* key_points, uint64_t n) const
        {
            for (uint64_t i = 1; i < n; ++i)
                if (key_points[i] <= key_points[i-1])
                    return false;
            return true;
        }
        void divide(uint64_t x, uint64_t& round_down, uint64_t& remainder,
                    uint64_t& round_up) const
        {
            auto ptr = std::upper_bound(key_points, key_points + n, x);
            assert(ptr > key_points);
            assert(ptr[-1] <= x && x < ptr[0]);
            auto i = ptr - key_points;
            round_down = i - 1;
            remainder = x - ptr[-1];
            round_up = (remainder > 0 ? i : round_down);
        }
        uint64_t multiply(uint64_t i, uint64_t x) const
        {
            return key_points[i] + x;
        }
        uint64_t get_length(uint64_t i) const
        {
            assert(i < n - 1);
            return key_points[i+1] - key_points[i];
        }
    };
}

