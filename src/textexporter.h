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

#pragma once
#include <photon/common/alog.h>
#include <photon/common/conststr.h>
#include <photon/thread/thread.h>
#include <string.h>

#include <string>
#include <type_traits>
#include <utility>

namespace photon {
namespace exporter {

template <typename TSName>
struct Label {
    using name = TSName;
    static constexpr decltype(auto) tpl() {
        return name().concat(TSTRING("=\"`\""));
    }
};

template <typename TS>
decltype(auto) label(TS) {
    return Label<TS>();
}

template <typename... Labels>
struct LabelGroup;

template <typename Label, typename... Labels>
struct LabelGroup<Label, Labels...> {
    static constexpr decltype(auto) tpl() {
        return TSTRING("{")
            .concat(
                ConstString::make_tstring_array(Label::tpl(), Labels::tpl()...)
                    .template join<','>())
            .concat(TSTRING("}"));
    }
};

template <>
struct LabelGroup<> {
    static constexpr decltype(auto) tpl() { return ConstString::TString<>(); }
};

template <typename S>
struct MType {
    template <typename NAME>
    static constexpr decltype(auto) _render() {
        return ConstString::make_tstring_array(TSTRING("# TYPE"), NAME(), S())
            .template join<' '>();
    }
};

template <typename S>
struct MHelp {
    template <typename NAME>
    static constexpr decltype(auto) _render() {
        return ConstString::make_tstring_array(TSTRING("# HELP"), NAME(), S())
            .template join<' '>();
    }
};

template <typename Name, typename Labels = LabelGroup<>,
          typename MType = MType<ConstString::TString<>>,
          typename MHelp = MHelp<ConstString::TString<>>>
struct PrometheusMetric {
    static constexpr size_t BufferSize = 8 * 1024;

    static constexpr decltype(auto) tpl() {
        return Name().concat(Labels::tpl()).concat(TSTRING(" ` `"));
    }

    template <typename Split, typename Arg, typename... Args>
    void _render(char* buf, size_t space, double val, Arg&& arg,
                 Args&&... args) {
        if (Split::Array::size > 0) {
            const auto view = Split::Array::views[0];
            if (view.size() < space) {
                memcpy(buf, view.data(), view.size());
                buf += view.size();
                space -= view.size();
            }
        }
        std::string_view v(std::forward<Arg>(arg));
        if (v.size() < space) {
            memcpy(buf, v.data(), v.size());
            buf += v.size();
            space -= v.size();
        }
        if (Split::Array::size > 0)
            _render<typename Split::Next, Args...>(buf, space, val,
                                                   std::forward<Args>(args)...);
    }

    template <typename Split>
    void _render(char* buf, size_t space, double val) {
        if (Split::Array::size > 0) {
            const auto view = Split::Array::views[0];
            if (view.size() < space) {
                memcpy(buf, view.data(), view.size());
                buf += view.size();
                space -= view.size();
            }
            _render<typename Split::Next>(buf, space, val);
            return;
        }

        auto v = std::to_string(val);
        auto ts = std::to_string(photon::now / 1000);
        if (v.size() < space) {
            memcpy(buf, v.data(), v.size());
            buf += v.size();
            space -= v.size();
        }
        if (ts.size() < space + 1) {
            *buf = ' ';
            buf++;
            space--;
            memcpy(buf, ts.data(), ts.size());
            buf += ts.size();
            space -= ts.size();
        }
        *buf = '\0';
    }

    template <typename... Args>
    std::string render(double val, Args&&... args) {
        char buffer[BufferSize];
        _render<ConstString::TSpliter<'`', 0, decltype(tpl())>>(
            buffer, sizeof(buffer), val, std::forward<Args>(args)...);
        return {buffer};
    }

    constexpr const char* type_str() {
        return MType::template _render<Name>().chars;
    }

    constexpr const char* help_str() {
        return MHelp::template _render<Name>().chars;
    }
};

template <typename Name, typename... Labels>
inline decltype(auto) metric(Name, Labels...) {
    return PrometheusMetric<Name, LabelGroup<Labels...>>();
}

template <typename Name>
inline decltype(auto) metric(Name) {
    return PrometheusMetric<Name, LabelGroup<>>();
}

template <typename Name, typename... LabelStrs>
inline constexpr decltype(auto) split_labels(
    ConstString::TStrArray<LabelStrs...> ts) {
    return LabelGroup<Label<LabelStrs>...>();
}

template <typename... TSS>
inline constexpr decltype(auto) label_helper(ConstString::TStrArray<TSS...>) {
    return LabelGroup<Label<TSS>...>();
}

template <typename TS>
inline constexpr decltype(auto) parse_metric_define(TS ts) {
    using name = decltype(ts.template cut<':'>().head.template strip<' '>());
    using comment = decltype(ts.template cut<'#'>().tail.template strip<' '>());
    using type = decltype(ts.template cut<'#'>()
                              .head.template cut<':'>()
                              .tail.template cut<'{'>()
                              .head.template strip<' '>());
    using labelstr = decltype(ts.template cut<'#'>()
                                  .head.template cut<'{'>()
                                  .tail.template cut<'}'>()
                                  .head.template strip<' '>());
    using labels =
        decltype(label_helper(labelstr().template split<',', ' '>()));
    return PrometheusMetric<name, labels, MType<type>, MHelp<comment>>();
}

}  // namespace exporter
}  // namespace photon

#define PROMMETRIC(...) \
    photon::exporter::parse_metric_define(TSTRING(#__VA_ARGS__))
