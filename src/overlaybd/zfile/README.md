# CRC

## The default order for CRC
If multiple features are included, the default order is:
1. DSA
2. AVX512
3. SSE4.2
4. software

## DSA

### Introduction

Intel® DSA is a high-performance data copy and transformation accelerator that will be
integrated into future Intel® processors, targeted for optimizing streaming data movement
and transformation operations common with applications for high-performance storage,
networking, persistent memory, and various data processing applications.

Intel® DSA replaces the Intel® QuickData Technology, which is a part of Intel® I/O 
Acceleration Technology.

The goal is to provide higher overall system performance for data mover and transformation
operations while freeing up CPU cycles for higher-level functions. Intel® DSA enables
high-performance data mover capability to/from volatile memory, persistent memory,
memory-mapped I/O, and through a Non-Transparent Bridge (NTB) device to/from remote volatile
and persistent memory on another node in a cluster. Enumeration and configuration are done
with a PCI Express* compatible programming interface to the Operating System (OS) and can be
controlled through a device driver.

Besides the basic data mover operations, Intel® DSA supports a set of transformation
operations on memory. For example:

Generate and test CRC checksum, or Data Integrity Field (DIF) to support storage and
networking applications.
Memory Compare and delta generate/merge to support VM migration, VM Fast check-pointing, and
software-managed memory deduplication usages.

We did a performance comparison test, and the QPS calculated by CRC were 960124 and 7977434
respectively in the case of 4K size without and with DSA. About a nine-times improvement
using DSA in performance.

### Check DSA hardware
```bash
$ lspci -nn | grep 8086:0b25
```
 
If get any information, which mean the system have DSA hardware.

Function check_dsa() is used to ensure that system have DSA hardware.

### Dependencies
Ubuntu 20.04 : apt-get install -y libpci-dev
CentOS 8.4 : yum install -y pciutils-devel

### Unit test for DSA Calculate CRC
```bash
$ cmake -DENABLE_DSA=1 -DBUILD_TESTING=1 ..
$ make -j
$./output/zfile_test
```

## AVX512

### Introduction

Here we use Intel(R) Intelligent Storage Acceleration Library for AVX512.

Intel(R) Intelligent Storage Acceleration Library is a collection of optimized low-level functions targeting storage applications. ISA-L includes:

Erasure codes - Fast block Reed-Solomon type erasure codes for any encode/decode matrix in GF(2^8).
CRC - Fast implementations of cyclic redundancy check. Six different polynomials supported.
    iscsi32, ieee32, t10dif, ecma64, iso64, jones64.
Raid - calculate and operate on XOR and P+Q parity found in common RAID implementations.
Compression - Fast deflate-compatible data compression.
De-compression - Fast inflate-compatible data compression.
igzip - A command line application like gzip, accelerated with ISA-L.

### Check if the system have AVX512

__builtin_cpu_supports("avx512f") is used to check if the system have AVX512.

### Dependencies
Ubuntu 20.04 : apt-get install -y nasm
CentOS 8.4 : yum install -y nasm

### Unit test for DSA Calculate CRC
```bash
$ cmake -DENABLE_ISAL=1 -DBUILD_TESTING=1 ..
$ make -j
$./output/zfile_test
```

# Compression

## QAT

### Introduction
Accelerate encryption and compression to provide improved
efficiency, scalability and performance – for data in motion or
at rest, in-flight or in process. Intel QuickAssist Technology (Intel QAT)
can save cycles, time and space of applications as varied as networking
to enterprise, cloud to storage, content delivery to database.

### Check QAT hardware
```bash
$ lspci -nn | grep 8086:4940
```

### Dependencies
Ubuntu 20.04 : apt-get install -y libpci-dev
CentOS 8.4 : yum install -y pciutils-devel

### Unit test for QAT compression and decompression
```bash
$ cmake -DENABLE_QAT=1 -DBUILD_TESTING=1 ..
$ make -j
$./output/zfile_test
```