#pragma once
#include <photon/common/utility.h>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace App {

struct ConfigGroup : public YAML::Node {
    ConfigGroup() = default;
    ConfigGroup(const YAML::Node &node) : YAML::Node(node){};
    ConfigGroup(const std::string &filename) {
        parseYAML(std::move(filename));
    }

    void parseYAML(const std::string &fn) {
        Clone(YAML::LoadFile(fn));
    }

    static size_t charfilter(char *dst, const char *src, char extract, size_t maxlen = 256UL) {
        size_t i;
        for (i = 0; (*src) && i < maxlen; i++, src++) {
            while (*src == '-') {
                src++;
            }
            if (!(*src))
                break;
            dst[i] = *(src);
        }
        dst[i] = 0;
        return i;
    }
};

#define APPCFG_PARA(ParaName, ParaType, ...)                                                       \
    inline ParaType ParaName() const {                                                             \
        return operator[](#ParaName).as<ParaType>(__VA_ARGS__);                                    \
    }

#define APPCFG_CLASS using ConfigGroup::ConfigGroup;

// merge two yaml nodes
// generate new node with full data
static YAML::Node mergeConfig(const YAML::Node &lhs, const YAML::Node &rhs) {
    // if one of nodes is not type of map
    // just return rhs
    if (lhs.Type() != YAML::NodeType::Map || rhs.Type() != YAML::NodeType::Map)
        return YAML::Clone(rhs);
    // both are map, merge two maps
    YAML::Node ret = YAML::Clone(lhs);
    for (auto &node : rhs) {
        auto key = node.first.as<std::string>();
        if (ret[key].IsDefined()) {
            // if key exists in lhs, merge recursivily
            ret[key] = mergeConfig(lhs[key], node.second);
        } else {
            // just add as new key
            ret[key] = Clone(node.second);
        }
    }
    return ret;
}

} // namespace App

namespace YAML {

template <typename T>
struct convert {
    template <ENABLE_IF_BASE_OF(App::ConfigGroup, T)>
    static Node encode(const T &rhs) {
        return rhs;
    }

    template <ENABLE_IF_BASE_OF(App::ConfigGroup, T)>
    static bool decode(const Node &node, T &rhs) {
        rhs = T(node);
        return true;
    }
};

} // namespace YAML