# ZFile format
## Overview
ZFile is a generic compression format that realizes random read of the compressed
file and online decompression, providing the users with an illusion of reading the
original file. ZFile is not tied with overlaybd. Instead, it works with arbitary
underlay file.


| Section | Size (bytes) | Description |
|  :---:  |    :----:    | :---        |
| header  |      512     | file header |
|  data   |   variable   | compressed blocks of the original file |
|  dict   |   variable   | optional dictionary to assist decompression |
|  index  |   variable   | a jump table that stores the size of each compressed block, which can by easily transformed into offset of the block at runtime |
| trailer |      512     | file trailer (similar to header) |

## header
The format of header is described as below. All fields are little-endian.

|  Field  | Offset (bytes) | Size (bytes) | Description |
|  :---:  |    :----:      |    :----:    | :---        |
| magic0  |       0        |      8       | "ZFile\0\1" (and an implicit '\0') |
| magic1  |       8        |      16      | 74 75 6A 69, 2E 79 79 66, 40 41 6C 69, 62 61 62 61 |
|  size   |      24        |   uint32_t   | size of the header structure, excluding the tail padding |
| digest  |      28        |   uint32_t   | checksum for the range 28-511 bytes in header |
| flags   |      32        |   uint64_t   | bits for flags* (see later for details) |
| index_offset | 40        |   uint64_t   | index offset |
| index_size   | 48        |   uint64_t   | size of the index section, possibly compressed base on flags |
| original_file_size | 56  |   uint64_t   | size of the orignal file before compression |
| index_crc |    64        |   uint32_t   | checksum value of index |
| reserved|      68        |      4       | reserved space, should be 0 |
| block_size|    72        |   uint32_t   | size of each compression block |
| algo    |      76        |   uint8_t    | compression algorithm |
| level   |      77        |   uint8_t    | compression level |
| use_dict|      78        |     bool     | whether use dictionary |
| reserved|      79        |      5       | reserved space, should be 0 |
| dict_size    | 84        |   uint32_t   | size of the dictionary section, 0 for non-existence |
| verify  |      88        |     bool     | whether these exists a 4-byte CRC32 checksum following each compressed block |
| reserved|      89       |     423    | reserved space for future use (offset 89 ~ 511), should be 0 |

**flags:**

|    Field    | Offset (bits) | Description |
|    :---:    |    :----:     | :---        |
|  is_header  |       0       | header (1) or trailer (0) |
|     type    |       1       | this is a data file (1) or index file (0) |
|    sealed   |       2       | this file is sealed (1) or not (0) |
| info_valid  |       3       | information validity of the fields *after* flags (they were initially invalid (0) after creation; and readers must resort to trailer when they meet such headers) |
|    digest   |       4       | the digest of this header/trailer has been recorded in the digest field |
| index_comperssion | 5       | whether the index has been compressed(1) or not(0) |
|   reserved  |       6~63    | reserved for future use; must be 0s |


## index
The index section is a table of (uint32_t) compressed size of each data block.
The whole section may be compressed with the same compression algorithm and
level.

## trailer
An updated edition of header, in the same format. Trailer is useful in
append-only storage during creation of the blob. Use trailer whenever
possible.
