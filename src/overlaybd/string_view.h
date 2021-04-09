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

// a header file to include std::string_view (of C++14) in c++11 mode,
// in a uniform way, for both gcc and clang

#if __cplusplus > 201700
#include <string_view>
#elif defined __clang__
#include <experimental/string_view>
#else
#pragma push_macro("__cplusplus")
#undef __cplusplus
#define __cplusplus 201402L
#include <experimental/string_view>
#pragma pop_macro("__cplusplus")
namespace std {
using string_view = std::experimental::string_view;
}
#endif
