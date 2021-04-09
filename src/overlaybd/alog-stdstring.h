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
