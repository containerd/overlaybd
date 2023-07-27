# Overlaybd layer blob format
## Overview
Each layer blob consists of 4 sections, namely header, data, index and trailer,
as described below.

| Section | Size (bytes) | Description |
|  :---:  |    :----:    | :---        |
| header  |     4096     | file header |
|  data   |   variable   | raw data (over) written in the layer |
|  index  |   variable   | a table that associates logical block addressing (LBA) with raw data |
| trailer |     4096     | file trailer (similar to header) |

## header
The format of header is described as below. All fields are little-endian.

|  Field  | Offset (bytes) | Size (bytes) | Description |
|  :---:  |    :----:      |    :----:    | :---        |
| magic0  |       0        |      8       | "LSMT\0\1\2" (and an implicit '\0') |
| magic1  |       8        |      16      | 65 7E 63 D2, 94 44 08 4C, A2 D2 C8 EC, 4F CF AE 8A |
|  size   |      24        |   uint32_t   | size of the header struct (390), excluding the tail padding |
| flags   |      28        |   uint32_t   | bits for flags* (see later for details) |
| index_offset | 32        |   uint64_t   | index offset |
| index_size   | 40        |   uint64_t   | index size |
| virtual_size | 48        |   uint64_t   | virtual size of the image that users see |
| uuid    |      56        |      37      | a string that identifies this blob |
| parent_uuid  | 93        |      37      | a string that identifies the parent (previous) blob |
| from    |      130       |   uint8_t    | deprecated |
| to      |      131       |   uint8_t    | deprecated |
| version |      132       |   uint8_t    | version of this blob |
| sub_version  | 133       |   uint8_t    | sub-version of this blob |
| user_tag     | 134       |     256      | commit message (user-defined text) |
| reserved     | 390       |     3706     | reserved space for future use (offset 390 ~ 4095), should be 0 |

**flags:**

|    Field    | Offset (bits) | Description |
|    :---:    |    :----:     | :---        |
|  is_header  |       0       | header (1) or trailer (0) |
|     type    |       1       | this is a data file (1) or index file (0) |
|    sealed   |       2       | this file is sealed (1) or not (0) |
|   gc_layer  |       3       | this is a gc layer (1) or normal layer (0) |
|  sparse_rw  |       4       | this is a sparse rw layer |
| info_valid  |       5       | information validity of the fields *after* flags (they were initially invalid (0) after creation; and readers must resort to trailer when they meet such headers) |
|   reserved  |      6~31     | reserved for future use; must be 0s |


## raw data
This section do not have a perticular format. The data stored in this section is
supposed to be referenced by the index entries. And it's possible for this section
to contain garbage data blocks that are not referenced by the any entry of the
index.


## index
The index section is a table that associates logical block addressing (LBA) with
raw data. Its format is simply a sorted array of record entries. Each entry is
a 128-bit struct defined as below:

|  Field  | Offset (bits)  | Size (bits)  |     Type     | Description |
|  :---:  |    :----:      |    :----:    |    :----:    | :---        |
| offset  |       0        |      50      |   uint64_t   | logical block addressing (in unit of 512-byte sector) |
| length  |      50        |      14      |   uint32_t   | length of the mapping (in unit of 512-byte sector) |
| moffset |      64        |      55      |   uint64_t   | mapped block addressing in the blob (in unit of 512-byte sector) |
| zeroed  |      119       |      1       |     bool     | whether the block is all zero (without actual mapping)  |
|   tag   |      120       |      8       |   uint8_t    | runtime usage only, should be 0 on-disk |

## trailer
An updated edition of header, in the same format. Trailer is useful in
append-only storage during creation of the blob. Use trailer whenever
possible.