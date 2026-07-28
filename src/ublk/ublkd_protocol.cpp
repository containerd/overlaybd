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
#include "ublkd_protocol.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

int ublkd_parse_add(const std::string &body, UblkdAddRequest &req, std::string &err) {
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        err = "request body is not a JSON object";
        return -1;
    }
    if (!doc.HasMember("config") || !doc["config"].IsString() ||
        doc["config"].GetStringLength() == 0) {
        err = "missing required field: config";
        return -1;
    }
    req.config = doc["config"].GetString();
    if (doc.HasMember("dev_id")) {
        if (!doc["dev_id"].IsInt()) {
            err = "dev_id must be an integer";
            return -1;
        }
        req.dev_id = doc["dev_id"].GetInt();
    }
    if (doc.HasMember("queue_depth")) {
        if (!doc["queue_depth"].IsInt() || doc["queue_depth"].GetInt() < 1 ||
            doc["queue_depth"].GetInt() > 4096) {
            err = "queue_depth must be an integer in [1, 4096]";
            return -1;
        }
        req.queue_depth = doc["queue_depth"].GetInt();
    }
    if (doc.HasMember("instance_id")) {
        if (!doc["instance_id"].IsString()) {
            err = "instance_id must be a string";
            return -1;
        }
        req.instance_id = doc["instance_id"].GetString();
    }
    return 0;
}

int ublkd_parse_del(const std::string &body, int &dev_id, std::string &err) {
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        err = "request body is not a JSON object";
        return -1;
    }
    if (!doc.HasMember("dev_id") || !doc["dev_id"].IsInt() || doc["dev_id"].GetInt() < 0) {
        err = "missing or invalid field: dev_id";
        return -1;
    }
    dev_id = doc["dev_id"].GetInt();
    return 0;
}

int ublkd_parse_resize(const std::string &body, UblkdResizeRequest &req,
                       std::string &err) {
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        err = "request body is not a JSON object";
        return -1;
    }
    if (!doc.HasMember("dev_id") || !doc["dev_id"].IsInt() || doc["dev_id"].GetInt() < 0) {
        err = "missing or invalid field: dev_id";
        return -1;
    }
    req.dev_id = doc["dev_id"].GetInt();
    // 1..1M GB: large enough for any real image, small enough to catch typos
    if (!doc.HasMember("size_gb") || !doc["size_gb"].IsUint64() ||
        doc["size_gb"].GetUint64() < 1 || doc["size_gb"].GetUint64() > (1ULL << 20)) {
        err = "size_gb must be an integer in [1, 1048576]";
        return -1;
    }
    req.size_gb = doc["size_gb"].GetUint64();
    if (doc.HasMember("resize_fs")) {
        if (!doc["resize_fs"].IsBool()) {
            err = "resize_fs must be a boolean";
            return -1;
        }
        req.resize_fs = doc["resize_fs"].GetBool();
    }
    return 0;
}

static std::string dump(const rapidjson::Document &doc) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);
    return sb.GetString();
}

std::string ublkd_msg_ok() {
    return R"({"ok":true})";
}

std::string ublkd_msg_error(const std::string &error) {
    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("ok", false, a);
    doc.AddMember("error", rapidjson::Value(error.c_str(), a), a);
    return dump(doc);
}

std::string ublkd_msg_added(int dev_id, const std::string &path) {
    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("ok", true, a);
    doc.AddMember("dev_id", dev_id, a);
    doc.AddMember("dev", rapidjson::Value(path.c_str(), a), a);
    return dump(doc);
}

std::string ublkd_msg_list(const std::vector<UblkdDeviceInfo> &devices) {
    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("ok", true, a);
    rapidjson::Value arr(rapidjson::kArrayType);
    for (const auto &d : devices) {
        rapidjson::Value o(rapidjson::kObjectType);
        o.AddMember("dev_id", d.dev_id, a);
        o.AddMember("dev", rapidjson::Value(d.dev_path.c_str(), a), a);
        o.AddMember("config", rapidjson::Value(d.config.c_str(), a), a);
        o.AddMember("writable", d.writable, a);
        o.AddMember("state", rapidjson::Value(d.state.c_str(), a), a);
        arr.PushBack(o, a);
    }
    doc.AddMember("devices", arr, a);
    return dump(doc);
}

std::string ublkd_msg_ping(const std::string &version) {
    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("ok", true, a);
    doc.AddMember("version", rapidjson::Value(version.c_str(), a), a);
    return dump(doc);
}
