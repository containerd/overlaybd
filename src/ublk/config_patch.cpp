/*
   Copyright The Overlaybd Authors

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
#include "config_patch.h"

#include <cstdint>
#include <cstdio>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

std::string ublk_image_cache_key(const std::string &canonical_path) {
    // FNV-1a 64-bit: tiny, dependency-free, stable across runs and platforms;
    // collision odds are negligible for config paths on a single host
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : canonical_path) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

bool ublk_config_has_upper(const std::string &image_json) {
    rapidjson::Document doc;
    doc.Parse(image_json.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("upper"))
        return false;
    const auto &upper = doc["upper"];
    if (!upper.IsObject())
        return false;
    // mirrors init_image_file(): an upper is effective when any of its
    // layer paths is set (index/data for LSMT, target for turboOCI)
    for (const char *key : {"index", "data", "target"}) {
        if (upper.HasMember(key) && upper[key].IsString() &&
            upper[key].GetStringLength() > 0)
            return true;
    }
    return false;
}

int ublk_patch_cache_dirs(const std::string &base_json, const std::string &cache_dir,
                          std::string &out_json) {
    rapidjson::Document doc;
    doc.Parse(base_json.c_str());
    if (doc.HasParseError() || !doc.IsObject())
        return -1;

    auto &alloc = doc.GetAllocator();
    std::string registry_dir = cache_dir + "/registry_cache";
    std::string gzip_dir = cache_dir + "/gzip_cache";

    auto set_str = [&](rapidjson::Value &obj, const char *key, const std::string &val) {
        obj.RemoveMember(key);
        obj.AddMember(rapidjson::Value(key, alloc), rapidjson::Value(val.c_str(), alloc),
                      alloc);
    };

    // new-style cache config (ImageService uses it when cacheType is set)
    if (!doc.HasMember("cacheConfig"))
        doc.AddMember("cacheConfig", rapidjson::Value(rapidjson::kObjectType), alloc);
    set_str(doc["cacheConfig"], "cacheDir", registry_dir);
    // legacy top-level field (used when cacheConfig.cacheType is empty)
    set_str(doc, "registryCacheDir", registry_dir);
    // gzip cache has its own directory; only patch if the section exists
    if (doc.HasMember("gzipCacheConfig"))
        set_str(doc["gzipCacheConfig"], "cacheDir", gzip_dir);

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);
    out_json.assign(sb.GetString(), sb.GetSize());
    return 0;
}
