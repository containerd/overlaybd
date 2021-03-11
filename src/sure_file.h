/*
 * sure_file.h
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#pragma once
#include "overlaybd/fs/filesystem.h"

struct ImageFile;

extern "C" FileSystem::IFile *new_sure_file(FileSystem::IFile *src_file, ImageFile *image_file,
                                            bool ownership = true);
extern "C" FileSystem::IFile *new_sure_file_by_path(const char *file_path, int open_flags,
                                                    ImageFile *image_file, bool ownership = true);