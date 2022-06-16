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

namespace photon {
    namespace fs {
        class IFile;
    }
}
class ImageFile;

IFile *new_sure_file(IFile *src_file, ImageFile *image_file,
                                            bool ownership = true);
IFile *new_sure_file_by_path(const char *file_path, int open_flags,
                                                    ImageFile *image_file, bool ownership = true);