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
| index_size   | 48        |   uint64_t   | entry count when bit 5 is 0; compressed byte count when bit 5 is 1 |
| original_file_size | 56  |   uint64_t   | size of the orignal file before compression |
| index_crc |    64        |   uint32_t   | CRC32C of the stored index bytes (compressed bytes when bit 5 is 1) |
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
The whole section may be compressed with the same compression algorithm as the
data blocks (raw LZ4 block or ZSTD frame). The current implementation uses the
same codec settings as data compression: LZ4 default and ZSTD level 3.

When flag bit 5 is clear, `index_size` is the number of uint32_t entries and the
stored length is `index_size * 4`, preserving existing files. When bit 5 is set,
`index_size` is the compressed byte length. The decoded entry count is
`original_file_size / block_size + (original_file_size % block_size != 0)`;
the decoded byte length must equal exactly four times that count. Validate
block size, entry count, codec limits and file bounds before allocating buffers.
The uncompressed entry count must also match the original file size. In a data
file, the sum of the block lengths must end at `index_offset`.
`index_crc` covers the stored bytes and is checked before decompression when
flag bit 4 is set. Header/trailer structure sizes and offsets are unchanged.

Writers opt in using `CompressArgs::compress_index` or the zfile tool's
`--compress-index` flag. Empty indexes, indexes which do not shrink, and indexes
above the single-call codec size limit remain uncompressed. Compressed indexes
use signed-int-sized codec buffers (and LZ4's smaller input limit when applicable).
New readers support both forms. Older readers do not understand bit 5, so keep
compression disabled when files must be read by older deployments.

## trailer
An updated edition of header, in the same format. Trailer is useful in
append-only storage during creation of the blob. Use trailer whenever
possible.
