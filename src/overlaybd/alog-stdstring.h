/*
 * alog-stdstring.h
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
#include "alog.h"
#include <string>
#include "string_view.h"

class string_key;

struct ALogStringSTD : public ALogString {
public:
    ALogStringSTD(const std::string &s) : ALogString(s.c_str(), s.length()) {
    }
    ALogStringSTD(const std::string_view &sv) : ALogString(sv.begin(), sv.length()) {
    }
};

inline LogBuffer &operator<<(LogBuffer &log, const std::string &s) {
    return log << ALogStringSTD(s);
}

inline LogBuffer &operator<<(LogBuffer &log, const std::string_view &sv) {
    return log << ALogStringSTD(sv);
}

inline LogBuffer &operator<<(LogBuffer &log, const string_key &sv) {
    return log << ALogStringSTD((const std::string_view &)sv);
}
