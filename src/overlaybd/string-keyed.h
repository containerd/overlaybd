/*
 * string-keyed.h
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

#include "string_view.h"
#include <string.h>
#include <unordered_map>
#include <map>

// string_key is a string_view with dedicated storage,
// primarily used as stored keys in std::map and std::unordered_map,
// so as to accept string_view as query keys.
class string_key : public std::string_view {
public:
    explicit string_key(const string_key &rhs) : string_key(rhs.data(), rhs.size()) {
    }
    explicit string_key(string_key &&rhs) = delete;
    ~string_key() {
        free((void *)begin());
    }

    string_key &operator=(const string_key &rhs) = delete;
    string_key &operator=(string_key &&rhs) = delete;

protected:
    explicit string_key(const char *s, size_t n) : std::string_view((char *)malloc(n + 1), n) {
        auto ptr = (char *)begin();
        memcpy(ptr, s, n);
        ptr[n] = '\0';
    }
};

// The basic class for maps with string keys, aims to avoid temp
// std::string construction in queries, by accepting string_views.
// M is the underlying map, either std::map or std::unordered_map
template <class M>
class basic_map_string_key : public M {
public:
    using base = M;
    using base::base;
    using typename base::const_iterator;
    using typename base::iterator;
    using typename base::mapped_type;
    using typename base::size_type;

    using base::erase;

    using key_type = std::string_view;
    using value_type = std::pair<const key_type, mapped_type>;

    mapped_type &operator[](const key_type &k) {
        return base::operator[]((const string_key &)k);
    }
    mapped_type &at(const key_type &k) {
        return base::at((const string_key &)k);
    }
    const mapped_type &at(const key_type &k) const {
        return base::at((const string_key &)k);
    }
    iterator find(const key_type &k) {
        return base::find((const string_key &)k);
    }
    const_iterator find(const key_type &k) const {
        return base::find((const string_key &)k);
    }
    size_type count(const key_type &k) const {
        return base::count((const string_key &)k);
    }
    std::pair<iterator, iterator> equal_range(const key_type &k) {
        return base::equal_range((const string_key &)k);
    }
    std::pair<const_iterator, const_iterator> equal_range(const key_type &k) const {
        return base::equal_range((const string_key &)k);
    }
    template <class... Args>
    std::pair<iterator, bool> emplace(const key_type &k, Args &&...args) {
        return base::emplace((const string_key &)k, std::forward<Args>(args)...);
    }
    template <class... Args>
    iterator emplace_hint(const_iterator position, const key_type &k, Args &&...args) {
        return base::emplace_hint(position, (const string_key &)k, std::forward<Args>(args)...);
    }
    std::pair<iterator, bool> insert(const value_type &k) {
        return emplace(k.first, k.second);
    }
    template <class P>
    std::pair<iterator, bool> insert(P &&val) {
        return emplace(val.first, std::move(val.second));
    }
    iterator insert(const_iterator hint, const value_type &val) {
        return emplace_hint(hint, val.first, val.second);
    }
    template <class P>
    iterator insert(const_iterator hint, P &&val) {
        return emplace_hint(hint, val.first, std::move(val.second));
    }
    template <class InputIterator>
    void insert(InputIterator first, InputIterator last) {
        for (auto it = first; it != last; ++it)
            insert(*it);
    }
    void insert(std::initializer_list<value_type> il) {
        insert(il.begin(), il.end());
    }
    size_type erase(const std::string_view &k) {
        return base::erase((const string_key &)k);
    }
};

template <class T, class Hasher = std::hash<std::string_view>,
          class KeyEqual = std::equal_to<std::string_view>,
          class Alloc = std::allocator<std::pair<const string_key, T>>>
using unordered_map_string_key =
    basic_map_string_key<std::unordered_map<string_key, T, Hasher, KeyEqual, Alloc>>;

template <class T, class Pred = std::less<string_key>,
          class Alloc = std::allocator<std::pair<const string_key, T>>>
class map_string_key : public basic_map_string_key<std::map<string_key, T, Pred, Alloc>> {
public:
    using base = basic_map_string_key<std::map<string_key, T, Pred, Alloc>>;
    using base::base;
    using typename base::const_iterator;
    using typename base::iterator;
    using typename base::key_type;
    using typename base::mapped_type;
    using typename base::size_type;

    iterator lower_bound(const key_type &k) {
        return base::lower_bound((const string_key &)k);
    }
    const_iterator lower_bound(const key_type &k) const {
        return base::lower_bound((const string_key &)k);
    }
    iterator upper_bound(const key_type &k) {
        return base::upper_bound((const string_key &)k);
    }
    const_iterator upper_bound(const key_type &k) const {
        return base::upper_bound((const string_key &)k);
    }
};
