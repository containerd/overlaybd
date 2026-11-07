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

#include "cache_import.h"
#include "ublkd_protocol.h"
#include "../image_service.h"
#include <rapidjson/document.h>

void ublkd_handle_cache_import(ImageService *service, const std::string &body, int &code,
                               std::string &msg) {
    auto fail = [&](int status, const char *reason) {
        code = status;
        msg = ublkd_msg_error(reason);
    };
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("path") ||
        !doc["path"].IsString() || !doc.HasMember("url") || !doc["url"].IsString() ||
        !doc.HasMember("digest") || !doc["digest"].IsString() || !doc.HasMember("size") ||
        !doc["size"].IsInt64() || doc["size"].GetInt64() <= 0) {
        fail(400, "expected path, url, sha256 digest and positive size");
        return;
    }
    std::string path(doc["path"].GetString(), doc["path"].GetStringLength());
    std::string url(doc["url"].GetString(), doc["url"].GetStringLength());
    std::string digest(doc["digest"].GetString(), doc["digest"].GetStringLength());
    const int64_t size = doc["size"].GetInt64();
    auto result = service->import_local_blob(path, url, digest, size);
    switch (result.status) {
    case CacheImportStatus::ok: code = 200; break;
    case CacheImportStatus::invalid: code = 400; break;
    case CacheImportStatus::unsupported: code = 409; break;
    case CacheImportStatus::too_large: code = 413; break;
    case CacheImportStatus::busy: code = 429; break;
    case CacheImportStatus::unavailable: code = 503; break;
    }
    msg = code == 200 ? ublkd_msg_ok() : ublkd_msg_error(result.message);
}
