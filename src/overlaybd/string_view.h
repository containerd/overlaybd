/*
 * string_view.h
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

// a header file to include std::string_view (of C++14) in c++11 mode,
// in a uniform way, for both gcc and clang

#if __cplusplus > 201700
#include <string_view>
#elif defined __clang__
#include <experimental/string_view>
#else
#pragma push_macro("__cplusplus")
#undef __cplusplus
#define __cplusplus 201402L
#include <experimental/string_view>
#pragma pop_macro("__cplusplus")
namespace std {
using string_view = std::experimental::string_view;
}
#endif
