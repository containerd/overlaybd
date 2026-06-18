/*
 * qcow2converter.cpp - Convert QCOW2 images to LSMT format
 *
 * Reads a QCOW2 disk image and converts it into an LSMT layer blob
 * compatible with the OverlayBD project. Supports rootfs partition
 * extraction (MBR/GPT) and full-disk conversion.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <array>
#include <cerrno>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/photon.h>
#include "../overlaybd/zfile/zfile.h"
#include "../overlaybd/lsmt/file.h"
#include "CLI11.hpp"
#include "qcow2converter.h"

using namespace photon::fs;
using namespace LSMT;

// QCOW2 Constants

#define QCOW2_MAGIC             (('Q' << 24) | ('F' << 16) | ('I' << 8) | 0xfb)
#define QCOW2_VERSION           2
#define QCOW2_VERSION3          3

#define QCOW2_OFLAG_COPIED      (1ULL << 63)
#define QCOW2_OFLAG_COMPRESSED  (1ULL << 62)
#define QCOW2_OFLAG_ZERO        (1ULL << 0)   // v3 only, standard L2 (not extended_l2)

#define QCOW2_INCOMPAT_DIRTY            (1ULL << 0)
#define QCOW2_INCOMPAT_CORRUPT          (1ULL << 1)
#define QCOW2_INCOMPAT_DATA_FILE        (1ULL << 2)
#define QCOW2_INCOMPAT_COMPRESSION_TYPE (1ULL << 3)
#define QCOW2_INCOMPAT_EXTL2            (1ULL << 4)

#define QCOW2_INCOMPAT_SUPPORTED_MASK   (QCOW2_INCOMPAT_DIRTY | QCOW2_INCOMPAT_CORRUPT | QCOW2_INCOMPAT_EXTL2)
#define QCOW2_L1E_OFFSET_MASK           0x00fffffffffffe00ULL
#define QCOW2_DEFAULT_CLUSTER_BITS      16
// QCOW2 Header (v2: 72 bytes)
struct Qcow2Header {
    uint32_t magic;                  // QCOW_MAGIC
    uint32_t version;                // 2 or 3

    uint64_t backing_file_offset;    // offset to backing file name string
    uint32_t backing_file_size;      // length of backing file name

    uint32_t cluster_bits;           // cluster size = 1 << cluster_bits
    uint64_t size;                   // virtual disk size in bytes
    uint32_t crypt_method;           // 0 = none, 1 = AES

    uint32_t l1_size;                // number of entries in L1 table
    uint64_t l1_table_offset;        // offset of L1 table in file

    uint64_t refcount_table_offset;  // offset of refcount table
    uint32_t refcount_table_clusters;// number of clusters for refcount table
    uint32_t nb_snapshots;           // number of snapshots
    uint64_t snapshots_offset;       // offset of snapshot table
} __attribute__((packed));

// QCOW2 v3 Header Extension (additional 32 bytes)
struct Qcow2HeaderV3 {
    // v2 fields (72 bytes)
    Qcow2Header v2;

    // v3 additional fields
    uint64_t incompatible_features;  // incompatible feature bits
    uint64_t compatible_features;    // compatible feature bits
    uint64_t autoclear_features;     // autoclear feature bits
    uint32_t refcount_order;         // refcount block entry width in bits
    uint32_t header_length;          // length of header structure
} __attribute__((packed));

static_assert(sizeof(Qcow2Header) == 72, "Qcow2Header size mismatch");
static_assert(sizeof(Qcow2HeaderV3) == 104, "Qcow2HeaderV3 size mismatch");

struct ClusterMapping {
    uint64_t logical_offset;   // logical byte offset in virtual disk
    uint64_t physical_offset;  // byte offset in qcow2 file (0 = unallocated/zero)
    uint32_t cluster_size;     // size of this cluster in bytes
    bool     is_zero;          // true if cluster reads as zero
    bool     is_unallocated;
    bool     is_compressed;    // true if cluster is compressed in qcow2
    uint64_t compressed_size;

    ClusterMapping() : logical_offset(0), physical_offset(0),
                       cluster_size(0), is_zero(false), is_unallocated(false), is_compressed(false), compressed_size(0) {}
};

class Qcow2Reader {
public:
    Qcow2Reader() : fd_(-1), cluster_size_(0), cluster_bits_(0),
                    l2_size_(0), l2_entry_size_(8), extended_l2_(false), virtual_size_(0) {}
    ~Qcow2Reader() { close(); }

    bool open(const char *path);
    void close();
    std::vector<ClusterMapping> get_cluster_mappings();
    ssize_t read_data(uint64_t logical_offset, void *buf, size_t count);

    uint64_t virtual_size() const { return virtual_size_; }
    uint32_t cluster_size()  const { return cluster_size_; }
    uint32_t cluster_bits()  const { return cluster_bits_; }

private:
    int      fd_;
    uint32_t cluster_size_;
    uint32_t cluster_bits_;
    uint32_t l2_size_;
    uint32_t l2_entry_size_;
    bool     extended_l2_;
    uint64_t virtual_size_;
    uint64_t l1_table_offset_;
    uint32_t l1_size_;
    std::vector<uint64_t> l1_table_;

    static uint64_t be64_to_cpu(const uint8_t *p) {
        return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
               ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
               ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
               ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
    }

    static uint32_t be32_to_cpu(const uint8_t *p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
    }

    std::vector<uint64_t> read_l2_table(uint64_t l2_offset);
};

// Disk partition parsing: MBR & GPT
// Identifies rootfs partitions by type + filesystem superblock magic

#pragma pack(push, 1)
struct MbrPartEntry {
    uint8_t  boot_flag;
    uint8_t  start_chs[3];
    uint8_t  type;            // 0x83=Linux, 0x82=swap, 0xEE=GPT
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t sectors;
};

struct MbrSector {
    uint8_t      bootstrap[446];
    MbrPartEntry parts[4];
    uint16_t     signature;   // 0xAA55
};
#pragma pack(pop)
static_assert(sizeof(MbrSector) == 512, "MBR must be 512 bytes");

// GPT header (LBA 1)
#pragma pack(push, 1)
struct GptHeader {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  guid[16];
    uint64_t entry_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc32;
};

struct GptPartEntry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
};
#pragma pack(pop)
static_assert(sizeof(GptPartEntry) == 128, "GPT entry must be 128 bytes");

// Known GPT type GUIDs (mixed-endian as stored on disk)
constexpr std::array<uint8_t, 16> GPT_LINUX_FS = {
    0xAF, 0x3D, 0xC6, 0x0F,  0x83, 0x84,  0x72, 0x47,
    0x8E, 0x79,  0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
constexpr std::array<uint8_t, 16> GPT_LINUX_SWAP = {
    0x6D, 0xFD, 0x57, 0x06,  0xAB, 0xA4,  0xC4, 0x43,
    0x84, 0xE5,  0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F
};
constexpr std::array<uint8_t, 16> GPT_EFI_SYSTEM = {
    0x28, 0x73, 0x2A, 0xC1,  0x1F, 0xF8,  0xD2, 0x11,
    0xBA, 0x4B,  0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
constexpr std::array<uint8_t, 16> GPT_BIOS_BOOT = {
    0x48, 0x61, 0x68, 0x21,  0x49, 0x64,  0x6F, 0x6E,
    0x74, 0x4E,  0x65, 0x65, 0x64, 0x45, 0x46, 0x49
};

// Partition info
struct PartitionInfo {
    uint64_t start_byte;
    uint64_t size_bytes;
    int      type_code;
    std::string label;
    bool     is_rootfs;

    PartitionInfo() : start_byte(0), size_bytes(0), type_code(0), is_rootfs(false) {}
};

static bool is_linux_mbr_type(uint8_t type) {
    return type == 0x83;
}

static bool is_excluded_mbr_type(uint8_t type) {
    return type == 0x82 || type == 0x05 || type == 0x0F ||
           type == 0xEF || type == 0x00;
}

static bool guid_match(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}

// Detect filesystem by superblock magic bytes
static bool detect_filesystem(const uint8_t *data, size_t size) {
    if (size < 4096) return false;

    // ext2/3/4: magic 0xEF53 at offset 0x438
    if (size > 0x43A) {
        uint16_t ext_magic = static_cast<uint16_t>(data[0x438]) | (static_cast<uint16_t>(data[0x439]) << 8);
        if (ext_magic == 0xEF53) return true;
    }

    // xfs: magic "XFSB" at offset 0
    if (size >= 4 && memcmp(data, "XFSB", 4) == 0) return true;

    // btrfs: magic "_BHRfS_M" at offset 0x10040 (requires >= 65608 bytes)
    if (size >= 0x10048 && memcmp(data + 0x10040, "_BHRfS_M", 8) == 0) return true;

    // f2fs: magic 0xF2F52010 at offset 0x400
    if (size > 0x404) {
        uint32_t f2fs_magic = static_cast<uint32_t>(data[0x400]) | (static_cast<uint32_t>(data[0x401]) << 8) |
                              (static_cast<uint32_t>(data[0x402]) << 16) | (static_cast<uint32_t>(data[0x403]) << 24);
        if (f2fs_magic == 0xF2F52010) return true;
    }

    return false;
}

// PartitionFilter: manages rootfs byte ranges for selective conversion
class PartitionFilter {
public:
    PartitionFilter() : enabled_(true) {}  // default: extract rootfs only

    void set_enabled(bool v) { enabled_ = v; }
    bool enabled() const { return enabled_; }

    // Parse disk image to find rootfs partitions
    bool scan(Qcow2Reader &reader) {
        partitions_.clear();
        ranges_.clear();

        if (!enabled_) return true;  // full-disk mode: no filtering

        // Read first 512 bytes (MBR sector)
        uint8_t sector0[512];
        ssize_t n = reader.read_data(0, sector0, 512);
        if (n < 512) {
            fprintf(stderr, "WARNING: Cannot read MBR sector, converting full image\n");
            enabled_ = false;
            return true;
        }

        const auto *mbr = reinterpret_cast<const MbrSector *>(sector0);

        // Check MBR signature
        if (mbr->signature != 0xAA55) {
            fprintf(stderr, "WARNING: No valid MBR signature, converting as raw image\n");
            enabled_ = false;
            return true;
        }

        // Check for GPT protective MBR (type 0xEE)
        bool has_gpt = false;
        for (int i = 0; i < 4; i++) {
            if (mbr->parts[i].type == 0xEE) { has_gpt = true; break; }
        }

        if (has_gpt) {
            parse_gpt(reader);
        } else {
            parse_mbr(reader, mbr);
        }

        // Build byte ranges for rootfs partitions
        for (const auto &p : partitions_) {
            if (p.is_rootfs) {
                ranges_.push_back({p.start_byte, p.start_byte + p.size_bytes});
            }
        }

        if (ranges_.empty()) {
            // No partition found; try detecting filesystem on raw disk
            std::vector<uint8_t> probe(66000);  // large enough for btrfs detection
            n = reader.read_data(0, probe.data(), probe.size());
            if (n > 0 && detect_filesystem(probe.data(), static_cast<size_t>(n))) {
                printf("  No partition table, but detected filesystem on raw disk\n");
                ranges_.push_back({0, reader.virtual_size()});
            } else {
                fprintf(stderr, "WARNING: No rootfs partitions found, converting full image\n");
                enabled_ = false;
                return true;
            }
        }

        printf("RootFS byte ranges to convert:\n");
        for (const auto &r : ranges_) {
            printf("  [%lu, %lu) = %lu bytes\n",
                   static_cast<unsigned long>(r.start), static_cast<unsigned long>(r.end),
                   static_cast<unsigned long>(r.end - r.start));
        }

        return true;
    }

    // Check if [offset, offset+len) overlaps with any rootfs range
    bool overlaps_rootfs(uint64_t offset, uint64_t len) const {
        if (!enabled_) return true;  // full-disk: everything passes
        if (ranges_.empty()) return false;

        uint64_t end = offset + len;
        for (const auto &r : ranges_) {
            if (offset < r.end && end > r.start) {
                return true;  // overlap exists
            }
        }
        return false;
    }

    // Get the total virtual size of extracted rootfs
    uint64_t total_rootfs_size() const {
        if (!enabled_ || ranges_.empty()) return 0;
        uint64_t total = 0;
        for (const auto &r : ranges_) {
            total += r.end - r.start;
        }
        return total;
    }

    // Remap absolute disk offset to output-relative offset
    uint64_t to_output_offset(uint64_t abs_offset) const {
        if (!enabled_) return abs_offset;

        uint64_t cumulative = 0;
        for (const auto &r : ranges_) {
            if (abs_offset >= r.start && abs_offset < r.end) {
                return cumulative + (abs_offset - r.start);
            }
            cumulative += r.end - r.start;
        }
        return UINT64_MAX;
    }

private:
    struct ByteRange {
        uint64_t start;
        uint64_t end;
    };

    bool enabled_;
    std::vector<PartitionInfo> partitions_;
    std::vector<ByteRange> ranges_;

    void parse_mbr(Qcow2Reader &reader, const MbrSector *mbr) {
        printf("\nPartition table: MBR\n");

        for (int i = 0; i < 4; i++) {
            const auto &pe = mbr->parts[i];

            if (pe.type == 0x00 || pe.sectors == 0) continue;

            // Handle extended partition (skip)
            if (pe.type == 0x05 || pe.type == 0x0F) {
                printf("  /dev/sda%d: type=0x%02X (extended) - skipped\n",
                       i + 1, pe.type);
                continue;
            }

            PartitionInfo pi;
            pi.start_byte = static_cast<uint64_t>(pe.start_lba) * 512;
            pi.size_bytes = static_cast<uint64_t>(pe.sectors) * 512;
            pi.type_code  = pe.type;

            pi.label = "sda" + std::to_string(i + 1);

            // Check if rootfs candidate
            if (is_linux_mbr_type(pe.type)) {
                std::vector<uint8_t> probe(66000);
                ssize_t n = reader.read_data(pi.start_byte, probe.data(), probe.size());
                if (n > 0 && detect_filesystem(probe.data(), static_cast<size_t>(n))) {
                    pi.is_rootfs = true;
                    printf("  %s: type=0x%02X, start=%lu, size=%lu -> ROOTFS\n",
                           pi.label.c_str(), pe.type,
                           static_cast<unsigned long>(pi.start_byte), static_cast<unsigned long>(pi.size_bytes));
                } else {
                    printf("  %s: type=0x%02X, start=%lu, size=%lu -> LINUX (no FS detected)\n",
                           pi.label.c_str(), pe.type,
                           static_cast<unsigned long>(pi.start_byte), static_cast<unsigned long>(pi.size_bytes));
                }
            } else if (is_excluded_mbr_type(pe.type)) {
                printf("  %s: type=0x%02X, start=%lu, size=%lu -> SKIPPED\n",
                       pi.label.c_str(), pe.type,
                       static_cast<unsigned long>(pi.start_byte), static_cast<unsigned long>(pi.size_bytes));
            } else {
                printf("  %s: type=0x%02X, start=%lu, size=%lu\n",
                       pi.label.c_str(), pe.type,
                       static_cast<unsigned long>(pi.start_byte), static_cast<unsigned long>(pi.size_bytes));
            }

            partitions_.push_back(pi);
        }
    }

    void parse_gpt(Qcow2Reader &reader) {
        printf("\nPartition table: GPT\n");

        // Read GPT header (LBA 1 = byte 512)
        uint8_t gpt_hdr_buf[512];
        ssize_t n = reader.read_data(512, gpt_hdr_buf, 512);
        if (n < 512) {
            fprintf(stderr, "WARNING: Cannot read GPT header\n");
            return;
        }

        const auto *gpt_hdr = reinterpret_cast<const GptHeader *>(gpt_hdr_buf);

        // Verify GPT signature
        if (memcmp(&gpt_hdr->signature, "EFI PART", 8) != 0) {
            fprintf(stderr, "WARNING: GPT header signature invalid\n");
            return;
        }

        uint32_t entry_size = gpt_hdr->entry_size;
        if (entry_size < 128) entry_size = 128;

        printf("  GPT entries: %u, entry size: %u\n",
               gpt_hdr->num_entries, entry_size);

        // Read partition entries (starting at entry_lba * 512)
        uint64_t entries_offset = gpt_hdr->entry_lba * 512;
        size_t entries_bytes = static_cast<size_t>(gpt_hdr->num_entries) * entry_size;
        // Round up to 512
        entries_bytes = ((entries_bytes + 511) / 512) * 512;

        std::vector<uint8_t> entries_buf(entries_bytes);
        n = reader.read_data(entries_offset, entries_buf.data(), entries_bytes);
        if (n < static_cast<ssize_t>(entries_bytes)) {
            fprintf(stderr, "WARNING: Cannot read all GPT entries\n");
        }

        for (uint32_t i = 0; i < gpt_hdr->num_entries; i++) {
            const auto *gpe = reinterpret_cast<const GptPartEntry *>(
                entries_buf.data() + static_cast<size_t>(i) * entry_size);

            // Check if entry is unused (all-zero type GUID)
            bool unused = true;
            for (int j = 0; j < 16; j++) {
                if (gpe->type_guid[j] != 0) { unused = false; break; }
            }
            if (unused) continue;

            PartitionInfo pi;
            pi.start_byte = gpe->first_lba * 512;
            pi.size_bytes = (gpe->last_lba - gpe->first_lba + 1) * 512;
            pi.type_code  = 0xEE00;  // marker for GPT

            char label[64];
            int label_len = 0;
            for (int k = 0; k < 36 && gpe->name[k]; k++) {
                if (gpe->name[k] < 128)
                    label[label_len++] = static_cast<char>(gpe->name[k]);
            }
            label[label_len] = '\0';
            pi.label = label;

            // Determine partition type
            const char *type_str = "UNKNOWN";
            bool is_linux = false, is_unknown = true;

            if (guid_match(gpe->type_guid, GPT_LINUX_FS.data())) {
                type_str = "Linux FS";
                is_linux = true;
                is_unknown = false;
            } else if (guid_match(gpe->type_guid, GPT_LINUX_SWAP.data())) {
                type_str = "Linux Swap";
                is_unknown = false;
            } else if (guid_match(gpe->type_guid, GPT_EFI_SYSTEM.data())) {
                type_str = "EFI System";
                is_unknown = false;
            } else if (guid_match(gpe->type_guid, GPT_BIOS_BOOT.data())) {
                type_str = "BIOS Boot";
                is_unknown = false;
            }

            if (is_linux || is_unknown) {
                std::vector<uint8_t> probe(66000);
                ssize_t rn = reader.read_data(pi.start_byte, probe.data(), probe.size());
                if (rn > 0 && detect_filesystem(probe.data(), static_cast<size_t>(rn))) {
                    pi.is_rootfs = true;
                    printf("  /dev/%s: %s, start=%lu, size=%lu -> ROOTFS\n",
                           pi.label.c_str(), type_str,
                           static_cast<unsigned long>(pi.start_byte), static_cast<unsigned long>(pi.size_bytes));
                } else {
                    printf("  /dev/%s: %s, start=%lu, size=%lu -> %s\n",
                           pi.label.c_str(), type_str,
                           static_cast<unsigned long>(pi.start_byte), static_cast<unsigned long>(pi.size_bytes),
                           is_linux ? "LINUX (no FS detected)" : "SKIPPED (no FS detected)");
                }
            } else {
                printf("  /dev/%s: %s, start=%lu, size=%lu -> SKIPPED\n",
                       pi.label.c_str(), type_str,
                       static_cast<unsigned long>(pi.start_byte), static_cast<unsigned long>(pi.size_bytes));
            }

            partitions_.push_back(pi);
        }
    }
};

// Qcow2Reader Implementation

bool Qcow2Reader::open(const char *path) {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        fprintf(stderr, "ERROR: Cannot open qcow2 file '%s': %s\n", path, strerror(errno));
        return false;
    }

    // Read header
    Qcow2Header hdr;
    ssize_t n = ::pread(fd_, &hdr, sizeof(hdr), 0);
    if (n != sizeof(hdr)) {
        fprintf(stderr, "ERROR: Failed to read qcow2 header\n");
        return false;
    }

    // Convert from big-endian
    auto *raw = reinterpret_cast<uint8_t *>(&hdr);
    uint32_t magic   = be32_to_cpu(raw + 0);
    uint32_t version = be32_to_cpu(raw + 4);

    if (magic != QCOW2_MAGIC) {
        fprintf(stderr, "ERROR: Not a valid QCOW2 image (magic=0x%08x)\n", magic);
        return false;
    }
    if (version < 2 || version > 3) {
        fprintf(stderr, "ERROR: Unsupported QCOW2 version %d (expected 2 or 3)\n", version);
        return false;
    }

    virtual_size_  = be64_to_cpu(raw + 24);  // size field
    cluster_bits_  = be32_to_cpu(raw + 20);  // cluster_bits field
    if (cluster_bits_ < 9 || cluster_bits_ > 30) {
        fprintf(stderr, "ERROR: Invalid cluster_bits %u (must be 9..30)\n", cluster_bits_);
        return false;
    }
    cluster_size_  = 1U << cluster_bits_;
    l1_size_       = be32_to_cpu(raw + 36);  // l1_size field
    l1_table_offset_ = be64_to_cpu(raw + 40); // l1_table_offset field

    // Default: standard 8-byte L2 entries
    extended_l2_ = false;
    l2_entry_size_ = sizeof(uint64_t);

    // Read v3 header extension to check incompatible features
    if (version == 3) {
        uint8_t v3_ext[32];
        n = ::pread(fd_, v3_ext, sizeof(v3_ext), sizeof(Qcow2Header));
        if (n == static_cast<ssize_t>(sizeof(v3_ext))) {
            uint64_t incompat = be64_to_cpu(v3_ext);
            if (incompat & QCOW2_INCOMPAT_EXTL2) {
                extended_l2_ = true;
                l2_entry_size_ = 16;
                printf("  Extended L2:      enabled (16-byte entries)\n");
            }
            if (incompat & QCOW2_INCOMPAT_DIRTY) {
                printf("  WARNING: Image was not closed properly (dirty bit set)\n");
            }
            if (incompat & QCOW2_INCOMPAT_CORRUPT) {
                printf("  WARNING: Image is marked as corrupt\n");
            }
            // Reject incompatible features we don't support
            uint64_t unsupported = incompat & ~QCOW2_INCOMPAT_SUPPORTED_MASK;
            if (unsupported) {
                fprintf(stderr, "ERROR: Unsupported incompatible features: 0x%lx\n",
                        static_cast<unsigned long>(unsupported));
                return false;
            }
        }
    }

    l2_size_ = cluster_size_ / l2_entry_size_;

    printf("QCOW2 image opened:\n");
    printf("  Virtual size:  %lu bytes (%.2f GB)\n",
           static_cast<unsigned long>(virtual_size_), virtual_size_ / (1024.0 * 1024.0 * 1024.0));
    printf("  Cluster size:  %u bytes (2^%u)\n", cluster_size_, cluster_bits_);
    printf("  L1 table:      %u entries at offset %lu\n",
           l1_size_, static_cast<unsigned long>(l1_table_offset_));
    printf("  L2 entries:    %u per table\n", l2_size_);

    // Read L1 table
    l1_table_.resize(l1_size_);
    size_t l1_bytes = l1_size_ * sizeof(uint64_t);
    std::vector<uint8_t> l1_buf(l1_bytes);
    n = ::pread(fd_, l1_buf.data(), l1_bytes, l1_table_offset_);
    if (n != static_cast<ssize_t>(l1_bytes)) {
        fprintf(stderr, "ERROR: Failed to read L1 table\n");
        return false;
    }
    for (uint32_t i = 0; i < l1_size_; i++) {
        l1_table_[i] = be64_to_cpu(l1_buf.data() + i * 8);
    }

    return true;
}

void Qcow2Reader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::vector<uint64_t> Qcow2Reader::read_l2_table(uint64_t l2_offset) {
    std::vector<uint64_t> table(l2_size_);
    size_t l2_bytes = static_cast<size_t>(l2_size_) * l2_entry_size_;
    std::vector<uint8_t> buf(l2_bytes);

    ssize_t n = ::pread(fd_, buf.data(), l2_bytes, l2_offset);
    if (n != static_cast<ssize_t>(l2_bytes)) {
        fprintf(stderr, "ERROR: Failed to read L2 table at offset %lu\n",
                static_cast<unsigned long>(l2_offset));
        std::fill(table.begin(), table.end(), 0);
        return table;
    }

    if (extended_l2_) {
        // Extended L2 entries (16 bytes each):
        //   Bytes 0-7:  Standard cluster descriptor (big-endian, same format
        //               as non-extended entries, EXCEPT bit 0 is NOT the zero flag)
        //   Bytes 8-11: Sub-cluster allocation bitmap (little-endian)
        //   Bytes 12-15: Sub-cluster zero bitmap (little-endian)
        // We only need the standard descriptor (bytes 0-7) for cluster-level ops.
        for (uint32_t i = 0; i < l2_size_; i++) {
            table[i] = be64_to_cpu(buf.data() + i * 16);  // first 8 bytes = descriptor
        }
    } else {
        for (uint32_t i = 0; i < l2_size_; i++) {
            table[i] = be64_to_cpu(buf.data() + i * 8);
        }
    }
    return table;
}

std::vector<ClusterMapping> Qcow2Reader::get_cluster_mappings() {
    std::vector<ClusterMapping> mappings;
    uint64_t total_clusters = (virtual_size_ + cluster_size_ - 1) / cluster_size_;

    printf("Scanning %lu clusters...\n", static_cast<unsigned long>(total_clusters));

    for (uint32_t l1_idx = 0; l1_idx < l1_size_; l1_idx++) {
        uint64_t l1_entry = l1_table_[l1_idx];

        // L1 entry == 0: entire L2 range is unallocated
        if (l1_entry == 0) {
            uint64_t base_offset = static_cast<uint64_t>(l1_idx) * l2_size_ * cluster_size_;
            for (uint32_t j = 0; j < l2_size_; j++) {
                uint64_t logical = base_offset + static_cast<uint64_t>(j) * cluster_size_;
                if (logical >= virtual_size_) break;

                ClusterMapping cm;
                cm.logical_offset = logical;
                cm.physical_offset = 0;
                cm.cluster_size = cluster_size_;
                cm.is_zero = true;
                cm.is_unallocated = true;
                cm.is_compressed = false;
                cm.compressed_size = 0;
                mappings.push_back(cm);
            }
            continue;
        }

        // L1 entry & mask gives the L2 table offset
        uint64_t l2_offset = l1_entry & QCOW2_L1E_OFFSET_MASK;
        auto l2_table = read_l2_table(l2_offset);

        uint64_t base_offset = static_cast<uint64_t>(l1_idx) * l2_size_ * cluster_size_;
        for (uint32_t j = 0; j < l2_size_; j++) {
            uint64_t logical = base_offset + static_cast<uint64_t>(j) * cluster_size_;
            if (logical >= virtual_size_) break;

            uint64_t l2_entry = l2_table[j];
            ClusterMapping cm;
            cm.logical_offset = logical;
            cm.cluster_size = cluster_size_;

            if (l2_entry == 0) {
                cm.physical_offset = 0;
                cm.is_zero = true;
                cm.is_unallocated = true;
                cm.is_compressed = false;
                cm.compressed_size = 0;
            } else if (l2_entry & QCOW2_OFLAG_COMPRESSED) {
                // MUST check compressed (bit 62) BEFORE zero (bit 0):
                // compressed entries store byte offset in bits 0..x-1,
                // which can have bit 0 set (odd address).
                int shift = 62 - (cluster_bits_ - 8);
                uint64_t offset_mask = (1ULL << shift) - 1;
                cm.physical_offset = l2_entry & offset_mask;

                uint64_t sectors_mask = (1ULL << (cluster_bits_ - 8)) - 1;
                uint64_t nb_sectors = ((l2_entry >> shift) & sectors_mask) + 1;
                cm.compressed_size = nb_sectors * 512 - (cm.physical_offset & 511);

                cm.is_zero = false;
                cm.is_unallocated = false;
                cm.is_compressed = true;

            } else if (!extended_l2_ && (l2_entry & QCOW2_OFLAG_ZERO)) {
                // v3 standard L2 only; in extended L2 bit 0 is reserved
                cm.physical_offset = l2_entry & QCOW2_L1E_OFFSET_MASK;
                cm.is_zero = true;
                cm.is_unallocated = false;
                cm.is_compressed = false;
                cm.compressed_size = 0;
            } else {
                cm.physical_offset = l2_entry & QCOW2_L1E_OFFSET_MASK;
                cm.is_zero = false;
                cm.is_unallocated = false;
                cm.is_compressed = false;
                cm.compressed_size = 0;
            }
            mappings.push_back(cm);
        }
    }

    printf("  Total clusters:     %zu\n", mappings.size());
    return mappings;
}

ssize_t Qcow2Reader::read_data(uint64_t logical_offset, void *buf, size_t count) {
    size_t total_read = 0;
    uint8_t *dst = static_cast<uint8_t *>(buf);

    // Loop to handle cross-cluster reads (e.g. probe buffers > cluster_size)
    while (total_read < count) {
        uint64_t cur_offset = logical_offset + total_read;
        size_t remaining = count - total_read;

        uint64_t cluster_idx = cur_offset / cluster_size_;
        uint64_t offset_in_cluster = cur_offset % cluster_size_;
        size_t chunk = std::min(remaining, static_cast<size_t>(cluster_size_ - offset_in_cluster));

        uint32_t l1_idx = cluster_idx / l2_size_;
        uint32_t l2_idx = cluster_idx % l2_size_;

        if (l1_idx >= l1_size_) {
            memset(dst + total_read, 0, remaining);
            return total_read + remaining;
        }

        uint64_t l1_entry = l1_table_[l1_idx];
        if (l1_entry == 0) {
            memset(dst + total_read, 0, chunk);
            total_read += chunk;
            continue;
        }

        uint64_t l2_offset = l1_entry & QCOW2_L1E_OFFSET_MASK;
        auto l2_table = read_l2_table(l2_offset);

        if (l2_idx >= l2_table.size()) {
            memset(dst + total_read, 0, chunk);
            total_read += chunk;
            continue;
        }

        uint64_t l2_entry = l2_table[l2_idx];
        if (l2_entry == 0) {
            memset(dst + total_read, 0, chunk);
            total_read += chunk;
            continue;
        }

        if (l2_entry & QCOW2_OFLAG_COMPRESSED) {
            int shift = 62 - (cluster_bits_ - 8);
            uint64_t mask = (1ULL << shift) - 1;
            uint64_t coffset = l2_entry & mask;
            uint64_t size_mask = (1ULL << (cluster_bits_ - 8)) - 1;
            uint64_t nb_sectors = ((l2_entry >> shift) & size_mask) + 1;
            uint64_t compressed_size = nb_sectors * 512 - (coffset & 511);

            std::vector<uint8_t> comp_buf(static_cast<size_t>(compressed_size));
            ssize_t n = ::pread(fd_, comp_buf.data(), static_cast<size_t>(compressed_size), coffset);
            if (n != static_cast<ssize_t>(compressed_size)) {
                fprintf(stderr, "ERROR: read compressed cluster failed at byte %lu\n",
                        static_cast<unsigned long>(coffset));
                return -1;
            }

            z_stream strm = {};
            strm.next_in = comp_buf.data();
            strm.avail_in = static_cast<uInt>(compressed_size);

            int ret = inflateInit2(&strm, -MAX_WBITS);
            if (ret != Z_OK) {
                fprintf(stderr, "ERROR: inflateInit2 failed: %d\n", ret);
                return -1;
            }

            std::vector<uint8_t> decomp(cluster_size_);
            strm.next_out = decomp.data();
            strm.avail_out = static_cast<uInt>(cluster_size_);

            ret = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);
            if (ret != Z_STREAM_END) {
                fprintf(stderr, "ERROR: decompression failed at offset %lu, ret=%d\n",
                        static_cast<unsigned long>(cur_offset), ret);
                return -1;
            }

            memcpy(dst + total_read, decomp.data() + offset_in_cluster, chunk);
            total_read += chunk;
            continue;
        }

        // In extended L2, bit 0 is reserved (not zero flag)
        if (!extended_l2_ && (l2_entry & QCOW2_OFLAG_ZERO)) {
            memset(dst + total_read, 0, chunk);
            total_read += chunk;
            continue;
        }

        // Normal cluster
        uint64_t phys_offset = (l2_entry & QCOW2_L1E_OFFSET_MASK) + offset_in_cluster;
        ssize_t n = ::pread(fd_, dst + total_read, chunk, phys_offset);
        if (n < 0) {
            fprintf(stderr, "ERROR: read failed at physical offset %lu\n",
                    static_cast<unsigned long>(phys_offset));
            return -1;
        }
        total_read += n;
        if (static_cast<size_t>(n) < chunk) break;
    }
    return total_read;
}

// Write qcow2 clusters to an existing overlaybd ImageFile.
// Used by overlaybd-apply --from_qcow2.

int convert_qcow2_to_imgfile(const char *input_path, IFile *target,
                              uint32_t block_size, bool verbose,
                              bool extract_rootfs) {
    // Open QCOW2 image
    Qcow2Reader reader;
    if (!reader.open(input_path)) {
        return -1;
    }

    uint64_t virtual_size = reader.virtual_size();
    uint32_t cluster_size = reader.cluster_size();

    // Setup partition filter
    PartitionFilter filter;
    filter.set_enabled(extract_rootfs);
    if (extract_rootfs) {
        if (!filter.scan(reader)) {
            return -1;
        }
        if (!filter.enabled()) {
            printf("No rootfs partitions detected, converting entire image.\n");
        }
    }

    // Determine output virtual size (rounded up to block_size)
    uint64_t output_virtual_size = virtual_size;
    if (filter.enabled()) {
        output_virtual_size = filter.total_rootfs_size();
        if (output_virtual_size == 0) {
            fprintf(stderr, "ERROR: No rootfs data to extract\n");
            return -1;
        }
        output_virtual_size = ((output_virtual_size + block_size - 1) / block_size) * block_size;
        printf("\nRootFS output virtual size: %lu bytes (%.2f GB)\n",
               static_cast<unsigned long>(output_virtual_size),
               output_virtual_size / (1024.0 * 1024.0 * 1024.0));
    }

    printf("\nWriting qcow2 clusters to overlaybd ImageFile, block_size=%u...\n", block_size);

    auto mappings = reader.get_cluster_mappings();
    std::vector<uint8_t> cluster_buf(cluster_size);

    uint64_t total_clusters = 0;
    uint64_t allocated_clusters = 0;
    uint64_t zero_clusters = 0;
    uint64_t compressed_clusters = 0;

    for (size_t i = 0; i < mappings.size(); i++) {
        const auto &cm = mappings[i];
        
        if (cm.is_unallocated) continue;

        // When rootfs filtering, skip clusters outside rootfs ranges
        if (filter.enabled() && !filter.overlaps_rootfs(cm.logical_offset, cm.cluster_size)) {
            continue;
        }

        // Determine output logical offset (remap for rootfs extraction)
        uint64_t out_offset = cm.logical_offset;
        if (filter.enabled()) {
            out_offset = filter.to_output_offset(cm.logical_offset);
            if (out_offset == UINT64_MAX) continue;
        }

        if (cm.is_zero) {
            // Zero cluster: punch hole directly, no need to read data
            int ret=target->fallocate(3, out_offset, cm.cluster_size);
            if (ret < 0) {
                fprintf(stderr, "ERROR: fallocate to ImageFile failed at offset %lu\n",
                        static_cast<unsigned long>(out_offset));
                return -1;
            }
        } else {
            ssize_t n = reader.read_data(cm.logical_offset, cluster_buf.data(), cm.cluster_size);
            if (n < 0) {
                fprintf(stderr, "ERROR: Failed to read cluster at logical offset %lu\n",
                        static_cast<unsigned long>(cm.logical_offset));
                return -1;
            }
            if (n < static_cast<ssize_t>(cm.cluster_size)) {
                std::fill(cluster_buf.begin() + n, cluster_buf.end(), 0);
            }
            ssize_t written = target->pwrite(cluster_buf.data(), cm.cluster_size, out_offset);
            if (written != static_cast<ssize_t>(cm.cluster_size)) {
                fprintf(stderr, "ERROR: pwrite to ImageFile failed at offset %lu (wrote %zd, expected %u)\n",
                        static_cast<unsigned long>(out_offset), written, cm.cluster_size);
                return -1;
            }
        }

        // Stats
        total_clusters++;
        if (cm.is_compressed) compressed_clusters++;
        else if (cm.is_zero) zero_clusters++;
        else allocated_clusters++;
    }

    if (verbose) printf("\n");

    printf("\nConversion complete.\n");
    printf("  Total clusters:       %lu\n", static_cast<unsigned long>(total_clusters));
    printf("  Allocated clusters:   %lu\n", static_cast<unsigned long>(allocated_clusters));
    printf("  Zero clusters:        %lu\n", static_cast<unsigned long>(zero_clusters));
    printf("  Compressed clusters:  %lu (qcow2 internal)\n",
           static_cast<unsigned long>(compressed_clusters));

    return 0;
}

// Main Entry Point (commented out - invoked from overlaybd-apply)

// int main(int argc, char *argv[]) {
//     CLI::App app("Convert a QCOW2 disk image to ZFile compressed format (OverlayBD ZFile).");

//     std::string input_path;
//     std::string output_path;
//     uint32_t block_size = DEFAULT_BLOCK_SIZE;
//     bool verbose = false;
//     std::string algorithm = "lz4";
//     int compress_threads = 1;

//     app.add_option("input", input_path, "Input QCOW2 image file")
//         ->required()
//         ->check(CLI::ExistingFile);
//     app.add_option("output", output_path, "Output ZFile compressed file")
//         ->required();

//     app.add_option("--block-size", block_size,
//                    "Compression block size in bytes (512-65536, 512-aligned)")
//         ->default_val(DEFAULT_BLOCK_SIZE)
//         ->check(CLI::Validator(
//             [](const std::string &input) -> std::string {
//                 uint32_t val = (uint32_t)std::stoul(input);
//                 if (val < 512 || val > 65536)
//                     return "Block size must be in [512, 65536]";
//                 if ((val & 511) != 0)
//                     return "Block size must be 512-aligned";
//                 return {};
//             }, "SIZE"));
//     app.add_option("--algorithm", algorithm,
//                    "Compression algorithm: lz4 or zstd (default: lz4)")
//         ->default_str("lz4")
//         ->check(CLI::IsMember({"lz4", "zstd"}));
//     app.add_option("--threads", compress_threads,
//                    "Number of compression threads (default: 1)")
//         ->default_val(1);
//     app.add_flag("-v,--verbose", verbose, "Show detailed progress");

//     bool extract_rootfs = true;
//     app.add_flag("--extract-rootfs,!--no-extract-rootfs", extract_rootfs,
//                  "Only extract rootfs partitions (default: on). "
//                  "Use --no-extract-rootfs to convert entire disk.");

//     app.footer("Example:\n"
//                "  qcow2converter disk.qcow2 disk.zfile --block-size 4096\n"
//                "  qcow2converter disk.qcow2 disk.zfile --algorithm zstd --threads 4\n"
//                "  qcow2converter disk.qcow2 disk.zfile --no-extract-rootfs");

//     CLI11_PARSE(app, argc, argv);

//     printf("=== QCOW2 to ZFile Converter (OverlayBD) ===\n");
//     printf("Input:    %s\n", input_path.c_str());
//     printf("Output:   %s\n", output_path.c_str());
//     printf("Block:    %u bytes\n", block_size);
//     printf("Algo:     %s\n", algorithm.c_str());
//     printf("Threads:  %d\n", compress_threads);
//     printf("Rootfs:   %s\n\n", extract_rootfs ? "yes" : "no (full disk)");

//     return convert_qcow2_to_zfile(input_path.c_str(), output_path.c_str(),
//                                   block_size, verbose, extract_rootfs,
//                                   algorithm, compress_threads);
// }
