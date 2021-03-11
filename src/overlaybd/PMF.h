/*
 * PMF.h
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

template <typename PF, typename T>
struct pmf_map {
    PF f;
    T *obj; // may be adjusted for virtual function call
};

template <typename PF, typename T, typename MF>
inline auto __get_mfa__(T *obj, MF f) -> pmf_map<PF, T> {
    struct PMF // Pointer to Member Function
    {
        union {
            uint64_t func_ptr, offset;
        };
        uint64_t this_adjustment;
        bool is_virtual() const {
            return offset & 1;
        }
        uint64_t get_virtual_function_address(void *&obj) const {
            (char *&)obj += this_adjustment;
            auto vtbl_addr = *(uint64_t *)obj;
            return *(uint64_t *)(vtbl_addr + offset - 1);
        }
        uint64_t get_function_address(void *&obj) const {
            return is_virtual() ? get_virtual_function_address(obj) : func_ptr;
        }
    };

    auto pmf = (PMF &)f;
    auto addr = pmf.get_function_address((void *&)obj);
    return pmf_map<PF, T>{(PF)addr, obj};
}

template <typename T, typename R, typename... ARGS>
inline auto get_member_function_address(T *obj, R (T::*f)(ARGS...))
    -> pmf_map<R (*)(T *, ARGS...), T> {
    typedef R (*PF)(T *, ARGS...);
    return __get_mfa__<PF>(obj, f);
}

template <typename T, typename R, typename... ARGS>
inline auto get_member_function_address(const T *obj, R (T::*f)(ARGS...) const)
    -> pmf_map<R (*)(const T *, ARGS...), const T> {
    typedef R (*PF)(const T *, ARGS...);
    return __get_mfa__<PF>(obj, f);
}
