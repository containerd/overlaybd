/*
 * config_util.h
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

#include "alog-stdstring.h"
#include "utility.h"
#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/pointer.h>
#include <rapidjson/prettywriter.h>

#include <string>
#include <type_traits>
#include <vector>

namespace ConfigUtils {

using Document = rapidjson::Document;
using Value = rapidjson::Value;
template <typename T>
using Array = rapidjson::GenericArray<false, T>;
template <typename T>
using Object = rapidjson::GenericObject<false, T>;
using Pointer = rapidjson::Pointer;

enum struct FORMAT { YAML = 0, JSON = 1, INI = 2 };

class Config : public Document {
public:
    Config() = default;
    Config(Document &&node) : Document(std::forward<Document>(node)){};
    Config(Config &&node) : Document(std::forward<Config>(node)){};
    Config(std::string &&filename, FORMAT fmt) {
        ParseJSON(std::move(filename));
    }

    bool ParseJSON(const std::string &fn) {
        FILE *fp = fopen(fn.c_str(), "r");
        DEFER(fclose(fp));
        char readBuffer[65536];
        rapidjson::FileReadStream frs(fp, readBuffer, sizeof(readBuffer));
        if (ParseStream(frs).HasParseError()) {
            LOG_ERROR("error parse json: `", fn);
            return false;
        }
        return true;
    }

    std::string DumpString() {
        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        Accept(writer);
        return buffer.GetString();
    }

    Config &Merge(const Value &rhs) {
        // if one of nodes is not type of map
        // just return rhs
        if (!IsObject() || !rhs.IsObject()) {
            CopyFrom(rhs, GetAllocator());
            return *this;
        }
        // both are map, merge two maps
        for (auto &node : rhs.GetObject()) {
            if (HasMember(node.name)) {
                // if key exists in lhs, merge recursivily
                Config m;
                m.CopyFrom((*this)[node.name], GetAllocator());
                m.Merge(node.value);
                (*this)[node.name].Swap(m);
            } else {
                // just add as new key
                (*this)[node.name].CopyFrom(node.value, GetAllocator());
            }
        }
        return *this;
    }
};

template <class T, typename Enable = void>
struct is_vector {
    static bool const value = false;
};

template <class T>
struct is_vector<
    T, typename std::enable_if<std::is_same<
           T, std::vector<typename T::value_type, typename T::allocator_type>>::value>::type> {
    static bool const value = true;
};

template <typename T, size_t N>
T GetResult(Document *j, const char (&path)[N], const T &default_value) {
    Value &val = GetValueByPointerWithDefault(*j, path, default_value);
    return val.Get<T>();
}

template <typename T, size_t N>
typename std::enable_if<!is_vector<T>::value && !std::is_base_of<Document, T>::value, T>::type
GetResult(Document *j, const char (&path)[N]) {
    Value *val = GetValueByPointer(*j, path);
    if (!val)
        return T();
    return val->Get<T>();
}

template <typename T, size_t N>
typename std::enable_if<!is_vector<T>::value && std::is_base_of<Document, T>::value, T>::type
GetResult(Document *j, const char (&path)[N]) {
    Value *val = GetValueByPointer(*j, path);
    Document ret;
    if (!val)
        return std::move(ret);
    ret.CopyFrom(*val, ret.GetAllocator());
    return std::move(ret);
}

template <typename T, size_t N>
typename std::enable_if<
    is_vector<T>::value && !std::is_base_of<Document, typename T::value_type>::value, T>::type
GetResult(

    Document *j, const char (&path)[N]) {
    Value *val = GetValueByPointer(*j, path);
    T ret;
    if (!val)
        return ret;
    for (auto &x : val->GetArray()) {
        ret.emplace_back(x.Get<typename T::value_type>());
    }
    return ret;
}

template <typename T, size_t N>
typename std::enable_if<
    is_vector<T>::value && std::is_base_of<Document, typename T::value_type>::value, T>::type
GetResult(

    Document *j, const char (&path)[N]) {
    Value *val = GetValueByPointer(*j, path);
    T ret;
    if (!val)
        return ret;
    for (auto &x : val->GetArray()) {
        Document node;
        node.CopyFrom(x, node.GetAllocator());
        ret.emplace_back(std::move(node));
    }
    return ret;
}

#define APPCFG_CLASS using ConfigUtils::Config::Config;
#define APPCFG_PARA(paraname, paratype, ...)                                                       \
    paratype paraname() {                                                                          \
        return ConfigUtils::GetResult<paratype>(this, "/" #paraname, ##__VA_ARGS__);               \
    }

} // namespace ConfigUtils
