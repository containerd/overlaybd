# CRC32C

ZFile uses Photon's CRC32C implementation for metadata and data blocks.
It uses the hardware implementation when the CPU supports both CRC32C and
carry-less multiplication, and Photon's software implementation otherwise.

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