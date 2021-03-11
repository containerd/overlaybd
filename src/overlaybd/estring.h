/*
 * estring.h
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
#include <stdio.h>
#include <assert.h>
#include <string>
#include <bitset>
#include <type_traits>
#include "string_view.h"
#include "alog-stdstring.h"
#include "utility.h"

struct charset : std::bitset<256> {
    charset() = default;
    charset(char ch) {
        this->set(ch, 1);
    }
    charset(const std::string_view &sv) {
        assert(!sv.empty());
        for (auto ch : sv)
            this->set(ch, 1);
    }
    template <size_t N>
    charset(const char (&s)[N]) : charset(std::string_view(s, N - 1)) {
    }

    bool test(char ch) const {
        return std::bitset<256>::test((unsigned char)ch);
    }
    bitset &set(char ch, bool value = true) {
        return std::bitset<256>::set((unsigned char)ch, value);
    }
};

class estring_view : public std::string_view {
public:
    using std::string_view::find_first_not_of;
    using std::string_view::find_first_of;
    using std::string_view::find_last_not_of;
    using std::string_view::find_last_of;
    using std::string_view::string_view;
    estring_view() : estring_view(nullptr, (size_type)0) {
    }
    estring_view(const char *begin, const char *end) : estring_view(begin, end - begin) {
    }
    size_t find_first_of(const charset &set) const {
        auto it = begin();
        for (; it != end(); ++it)
            if (set.test(*it))
                return it - begin();
        return npos;
    }
    size_t find_first_not_of(const charset &set) const {
        auto it = begin();
        for (; it != end(); ++it)
            if (!set.test(*it))
                return it - begin();
        return npos;
    }
    size_t find_last_of(const charset &set) const {
        auto it = rbegin();
        for (; it != rend(); ++it)
            if (set.test(*it))
                return &*it - &*begin();
        return npos;
    }
    size_t find_last_not_of(const charset &set) const {
        auto it = rbegin();
        for (; it != rend(); ++it)
            if (!set.test(*it))
                return &*it - &*begin();
        return npos;
    }
    operator std::string() {
        return std::string(data(), length());
    }
    std::string to_string() {
        return std::string(data(), length());
    }
    estring_view substr(size_type pos = 0, size_type count = npos) const {
        auto ret = std::string_view::substr(pos, count);
        return (estring_view &)ret;
    }
    estring_view trim(const charset &spaces = charset(" \t\r\n")) const {
        auto start = find_first_not_of(spaces);
        if (start == npos)
            return {};

        auto end = find_last_not_of(spaces);
        assert(end >= start);
        return substr(start, end - start + 1);
    }
#if __cplusplus < 202000L
    bool starts_with(estring_view x) {
        auto len = x.size();
        return length() >= len && memcmp(data(), x.data(), len) == 0;
    }
    bool ends_with(estring_view x) {
        auto len = x.size();
        return length() >= len && memcmp(&*end() - len, x.data(), len) == 0;
    }
#endif
    bool operator==(const std::string &rhs) {
        if (size() != rhs.size())
            return false;
        return memcmp(data(), rhs.data(), size()) == 0;
    }
};

class estring : public std::string {
public:
    using std::string::string;

    estring() = default;
    estring(const std::string &v) : std::string(v) {
    }
    estring(estring_view sv) : std::string(sv) {
    }
    estring(std::string_view sv) : std::string(sv) {
    }

    template <typename Separator>
    struct _split {
        const estring *str;
        Separator sep;
        bool consecutive_merge;

        class iterator {
        public:
            iterator() = default;
            iterator(const _split *host, size_t pos = 0) {
                _host = host;
                _part = _host->find_part(_host->str->data() + pos);
            }
            estring_view operator*() const {
                return _part;
            }
            estring_view remainder() const {
                auto it = *this;
                ++it;
                return {&*it._part.begin(), &*_host->str->end()};
            }
            iterator &operator++() {
                _part = _host->find_part(_part.end());
                return *this;
            }
            iterator &operator++(int) {
                auto ret = *this;
                ++(*this);
                return ret;
            }
            bool operator==(const iterator &rhs) const {
                return _part == rhs._part;
            }
            bool operator!=(const iterator &rhs) const {
                return !(*this == rhs);
            }

        protected:
            const _split *_host;
            estring_view _part;
        };

        const estring &get_sep_ref(const estring *) const {
            return *sep;
        }
        const estring &get_sep_ref(const estring &) const {
            return sep;
        }
        const charset &get_sep_ref(const charset *) const {
            return *sep;
        }
        const charset &get_sep_ref(const charset &) const {
            return sep;
        }

        estring_view find_part(const char *begin) const {
            auto end = &*str->end();
            auto &separator = get_sep_ref(sep);

            if (consecutive_merge) { // skip leading separators, if consecutive_merge
                auto pos = estring_view(begin, end).find_first_not_of(separator);
                if (pos == estring_view::npos) {
                    begin = end;
                } else {
                    begin += pos;
                }
            }

            if (begin >= end)
                return {};

            // count non-separators
            estring_view sv(begin, end);
            auto len = sv.find_first_of(separator);
            if (len == estring_view::npos)
                len = sv.size();

            return estring_view(begin, len);
        }

        iterator begin() const {
            return iterator(this);
        }
        iterator end() const {
            return iterator(this, str->size());
        }
        estring_view front() const {
            return *begin();
        }
        estring_view operator[](size_t i) const {
            // TODO: support for negative index
            if (i == 0)
                return front();

            auto it = begin();
            auto _end = end();
            while (i-- && it != _end)
                ++it;
            return *it;
        }
    };

    template <size_t N>
    _split<charset> split(const char (&s)[N], bool consecutive_merge = true) const {
        return {this, std::move(charset(s)), consecutive_merge};
    }
    _split<charset> split(charset &&sep, bool consecutive_merge = true) const {
        return {this, std::move(sep), consecutive_merge};
    }
    _split<const charset *> split(const charset &sep, bool consecutive_merge = true) const {
        return {this, &sep, consecutive_merge};
    }
    _split<std::string> split(std::string &&sep, bool consecutive_merge = true) const {
        return {this, std::move(sep), consecutive_merge};
    }
    _split<const std::string *> split(const std::string &sep, bool consecutive_merge = true) const {
        return {this, &sep, consecutive_merge};
    }
    _split<charset> split_lines(bool consecutive_merge = true) const {
        return split(charset("\r\n"), consecutive_merge);
    }
    estring_view view() const {
        return {c_str(), size()};
    }
    estring_view trim(const charset &spaces = charset(" \t\r\n")) const {
        return view().trim(spaces);
    }
#if __cplusplus < 202000L
    bool starts_with(estring_view x) {
        return view().starts_with(x);
    }
    bool ends_with(estring_view x) {
        return view().ends_with(x);
    }
#endif

    template <size_t N = 4096, typename... Ts>
    static estring snprintf(const Ts &...xs) {
        char buf[N];
        int n = ::snprintf(buf, N, xs...);
        return estring(buf, n < 0 ? 0 : n > (int)N ? N : n);
    }
};

namespace std {
template <>
struct hash<estring_view> {
    std::hash<std::string_view> hasher;

    size_t operator()(const estring_view &x) const {
        return hasher(x);
    }
};

template <>
struct hash<estring> {
    std::hash<std::string> hasher;

    size_t operator()(const estring &x) const {
        return hasher(x);
    }
};
} // namespace std

struct ALogEString : public ALogString {
public:
    ALogEString(const estring &s) : ALogString(s.c_str(), s.length()) {
    }
    ALogEString(const estring_view &sv) : ALogString(sv.data(), sv.length()) {
    }
};

inline LogBuffer &operator<<(LogBuffer &log, const estring &s) {
    return log << ALogEString(s);
}

inline LogBuffer &operator<<(LogBuffer &log, const estring_view &sv) {
    return log << ALogEString(sv);
}
