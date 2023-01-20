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
#include <photon/fs/filesystem.h>

// switch to local file after background download finished, and audit for local file pread
// operations. if initialized with local file, only audit for pread.
class ISwitchFile : public photon::fs::IFile {
public:
    virtual void set_switch_file(const char *filepath) = 0;
};

extern "C" ISwitchFile *new_switch_file(photon::fs::IFile *source, bool local = false,
                                        const char *filepath = nullptr);

