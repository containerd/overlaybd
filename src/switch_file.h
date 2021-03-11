/*
 * switch_file.h
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

#include "../overlaybd/fs/filesystem.h"

namespace FileSystem {
class ISwitchFile : public IFile {
public:
    virtual void set_switch_file(const char *filepath) = 0;
};

extern "C" ISwitchFile *new_switch_file(IFile *source);

} // namespace FileSystem