/*
  * exportfs.h
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

namespace FileSystem
{
    class IFile;
    class IFileSystem;
    class DIR;
    class IAsyncFile;
    class IAsyncFileSystem;
    class AsyncDIR;

    extern "C"
    {
        // The exports below wrap a sync file/fs/dir object into an
        // async/sync one for ourside world, assuming that the exported
        // will be used in other OS thread(s) (not necessarily), so
        // the exports will act accordingly in a thread-safe manner.

        int exportfs_init();
        int exportfs_fini();

        // create exports from sync file/fs/dir, obtaining their ownship, so deleting
        // the adaptor objects means deleting the async fs objects, too.
        IAsyncFile*         export_as_async_file(IFile* file);
        IAsyncFileSystem*   export_as_async_fs(IFileSystem* fs);
        AsyncDIR*           export_as_async_dir(DIR* dir);

        IFile*         export_as_sync_file(IFile* file);
        IFileSystem*   export_as_sync_fs(IFileSystem* fs);
        DIR*           export_as_sync_dir(DIR* dir);

        IFile*         export_as_easy_sync_file(IFile* file);
        IFileSystem*   export_as_easy_sync_fs(IFileSystem* fs);
        DIR*           export_as_easy_sync_dir(DIR* dir);
    }
}
