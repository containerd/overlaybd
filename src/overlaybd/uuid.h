/*
 * uuid.h
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

#include "uuid4.h"
#include "alog.h"

class UUID {
public:
    struct String {
        enum { LEN = 37 };
        char data[LEN];

        String() {
        }
        String(const UUID &rhs) {
            rhs.to_string(data, LEN);
        }
        String(const String &rhs) = default;

        String &operator=(const UUID &rhs) {
            rhs.to_string(data, LEN);
            return *this;
        }
        String &operator=(const String &rhs) = default;

        const char *c_str() const {
            return (const char *)data;
        }
        // check uuid string is valid.
        static bool is_valid(const char *in = nullptr) {
            return invalid_uuid4(const_cast<char *>(in)) == 0;
        }
    } __attribute__((packed));

    // parse a UUID-Format string.
    int parse(const char *in, size_t n) {
        if (uuid4_parse(const_cast<char *>(in), (char *)this) != 0) {
            LOG_ERROR_RETURN(0, -1, "invalid UUID format.");
        }
        return 0;
    }

    int parse(const String &str_uuid) {
        return parse((char *)&str_uuid, String::LEN);
    }

    int reset(const char *in, size_t n) {
        if (n != 16) {
            LOG_ERRNO_RETURN(0, -1, "invalid UUID format");
        }
        uuid4_copy((char *)this, const_cast<char *>(in));
        return 0;
    }

    int to_string(char *out, size_t len) const {
        if (len < String::LEN) {
            int x = String::LEN;
            LOG_ERROR_RETURN(ENOBUFS, x - len, "buffer size too slow, ` bytes required.", x);
        }
        uuid4_unparse_upper((char *)this, out);
        return 0;
    }

    void generate() {
        uuid4_generate((char *)this);
    }

    void clear() {
        uuid4_clear((char *)this);
    }

    bool is_null() const {
        return uuid4_is_null((char *)this);
    }

    bool operator==(const UUID &rhs) const {
        return memcmp((char *)&rhs, (char *)this, LEN) == 0;
    }

    bool operator!=(const UUID &rhs) {
        return !(*this == rhs);
    }

    UUID &operator=(const UUID &rhs) {
        uuid4_copy((char *)this, ((char *)&rhs));
        return *this;
    }

    uint32_t a;
    uint16_t b, c, d;
    ;
    uint8_t e[6];

    static const int LEN = 16;
} __attribute__((packed));

inline LogBuffer &operator<<(LogBuffer &log, const UUID::String &str_uuid) {
    log << str_uuid.c_str();
    return log;
}

inline LogBuffer &operator<<(LogBuffer &log, const UUID &uuid) {
    UUID::String str_uuid = uuid;
    log << str_uuid.c_str();
    return log;
}
