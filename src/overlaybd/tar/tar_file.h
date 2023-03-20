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

// tarfile and tarfs are not complete implementation of tar.
// only for overlaybd remote blob, which stores one blob file with tar header and trailer.
// used to skip header for file I/O.

photon::fs::IFileSystem *new_tar_fs_adaptor(photon::fs::IFileSystem *fs);

int is_tar_file(photon::fs::IFile *file);
photon::fs::IFile *new_tar_file_adaptor(photon::fs::IFile *file);
