/*
  * fiemap.h
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
#include <inttypes.h>

namespace FileSystem
{
    typedef uint32_t __u32;
    typedef uint64_t __u64;
    
    struct fiemap_extent {
        __u64 fe_logical;  /* logical offset in bytes for the start of
                            * the extent from the beginning of the file */
        __u64 fe_physical; /* physical offset in bytes for the start
                            * of the extent from the beginning of the disk */
        __u64 fe_length;   /* length in bytes for this extent */
        __u64 fe_logical_end() { return fe_logical + fe_length; }
        __u64 fe_reserved64[2];
        __u32 fe_flags;    /* FIEMAP_EXTENT_* flags for this extent */
        __u32 fe_reserved[3];
    };
    
    struct fiemap {
        __u64 fm_start;         /* logical offset (inclusive) at
                                 * which to start mapping (in) */
        __u64 fm_length;        /* logical length of mapping which
                                 * userspace wants (in) */
        __u32 fm_flags = 0;     /* FIEMAP_FLAG_* flags for request (in/out) */
        __u32 fm_mapped_extents;/* number of extents that were mapped (out) */
        __u32 fm_extent_count;  /* size of fm_extents array (in) */
        __u32 fm_reserved = 0;
        struct fiemap_extent fm_extents[0]; /* array of mapped extents (out) */

        fiemap(__u64 fm_start, __u64 fm_length, __u32 fm_extent_count) :
            fm_start(fm_start), fm_length(fm_length), fm_extent_count(fm_extent_count) { }
    };

    template<__u64 N>
    struct fiemap_t : public fiemap
    {
        struct fiemap_extent fm_extents[N];
        fiemap_t(__u64 fm_start, __u64 fm_length) : fiemap(fm_start, fm_length, N) { }
    };

#define FIEMAP_MAX_OFFSET   (~0ULL)
    
#define FIEMAP_FLAG_SYNC    0x00000001 /* sync file data before map */
#define FIEMAP_FLAG_XATTR   0x00000002 /* map extended attribute tree */
#define FIEMAP_FLAG_CACHE   0x00000004 /* request caching of the extents */
    
#define FIEMAP_FLAGS_COMPAT (FIEMAP_FLAG_SYNC | FIEMAP_FLAG_XATTR)
    
#define FIEMAP_EXTENT_LAST              0x00000001 /* Last extent in file. */
#define FIEMAP_EXTENT_UNKNOWN           0x00000002 /* Data location unknown. */
#define FIEMAP_EXTENT_DELALLOC          0x00000004 /* Location still pending.
                                                    * Sets EXTENT_UNKNOWN. */
#define FIEMAP_EXTENT_ENCODED           0x00000008 /* Data can not be read
                                                    * while fs is unmounted */
#define FIEMAP_EXTENT_DATA_ENCRYPTED    0x00000080 /* Data is encrypted by fs.
                                                    * Sets EXTENT_NO_BYPASS. */
#define FIEMAP_EXTENT_NOT_ALIGNED       0x00000100 /* Extent offsets may not be
                                                    * block aligned. */
#define FIEMAP_EXTENT_DATA_INLINE       0x00000200 /* Data mixed with metadata.
                                                    * Sets EXTENT_NOT_ALIGNED.*/
#define FIEMAP_EXTENT_DATA_TAIL         0x00000400 /* Multiple files in block.
                                                    * Sets EXTENT_NOT_ALIGNED.*/
#define FIEMAP_EXTENT_UNWRITTEN         0x00000800 /* Space allocated, but
                                                    * no data (i.e. zero). */
#define FIEMAP_EXTENT_MERGED            0x00001000 /* File does not natively
                                                    * support extents. Result
                                                    * merged for efficiency. */
#define FIEMAP_EXTENT_SHARED            0x00002000 /* Space shared with other
                                                    * files. */

}
