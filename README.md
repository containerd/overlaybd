# Overlaybd

Overlaybd is a novel layering block-level image format, which is design for container, secure container and applicable to virtual machine. And it is an open-source implementation of paper [DADI: Block-Level Image Service for Agile and Elastic Application Deployment. USENIX ATC'20"](https://www.usenix.org/conference/atc20/presentation/li-huiba).

Overlaybd has 2 core component:
* **Overlaybd**
  is a block-device based image format, provideing a merged view of a sequence of block-based layers as a virtual block device.

* **Zfile**
  is a compression file format which support seekalbe online decompression.

This repository is an implementation of overlaybd based on [TCMU](https://www.kernel.org/doc/Documentation/target/tcmu-design.txt).

Overlaybd can be used as the storage backend of [Accelerated Container Image](https://github.com/containerd/accelerated-container-image), which is a solution of remote container image by fetching image data on-demand without downloading and unpacking the whole image before the container starts.

Benefits from the universality of block-device, overlaybd is also a widely applicable image format for most runtime, including qemu/kvm and any other runtime supporting block or scsi api.

Overlaybd is a __non-core__ sub-project of containerd.

## Setup

### System Requirements

Overlaybd provides virtual block devices through TCMU, so the TCMU kernel module is required. TCMU is implemented in the Linux kernel and supported by most Linux distributions.

Check and load the target_core_user module.

```bash
modprobe target_core_user
```

### Install From RPM/DEB

You may download our RPM/DEB packages form [Release](https://github.com/containerd/overlaybd/releases) and install.

The binaries are install to `/opt/overlaybd/bin/`.

Run `/opt/overlaybd/bin/overlaybd-tcmu` and the log is stored in `/var/log/overlaybd.log`.

It is better to run `overlaybd-tcmu` as a service so that it can be restarted after unexpected crashes.

### Build From Source

#### Requirements

To build overlaybd from source code, the following dependencies are required:

* CMake >= 3.11

* gcc/g++ >= 7

* Libaio, libcurl, libnl3, glib2 and openssl runtime and development libraries.
  * CentOS/Fedora: `sudo yum install libaio-devel libcurl-devel openssl-devel libnl3-devel glib2-devel`
  * Debian/Ubuntu: `sudo apt install pkg-config libcurl4-openssl-dev libssl-dev libaio-dev libnl-3-dev libnl-genl-3-dev libglib2.0-dev libgflags-dev`

#### Build

You need git to checkout the source code:

```bash
git clone https://github.com/containerd/overlaybd.git
cd overlaybd
git submodule update --init
```

The whole project is managed by CMake. Binaries and resource files will be installed to `/opt/overlaybd/`.

```bash
mkdir build
cd build
cmake ..
make -j
sudo make install
```

If you want to use DSA hardware to accelerate CRC calculation.

```bash
cmake -D ENABLE_DSA=1 ..
```

If you want to use avx512 to accelerate CRC calculation.

```bash
cmake -D ENABLE_ISAL=1 ..
```

For more informations go to `overlaybd/src/overlaybd/fs/zfile/README.md`.

Finally, setup a systemd service for overlaybd-tcmu backstore.

```bash
sudo systemctl enable /opt/overlaybd/overlaybd-tcmu.service
sudo systemctl start overlaybd-tcmu
```

## Configuration

### overlaybd config
Default configure file `overlaybd.json` is installed to `/etc/overlaybd/`.

```json
{
    "logLevel": 1,
    "logPath": "/var/log/overlaybd.log",
    "registryCacheDir": "/opt/overlaybd/registry_cache",
    "registryCacheSizeGB": 1,
    "credentialFilePath": "/opt/overlaybd/cred.json",
    "ioEngine": 0,
    "download": {
        "enable": true,
        "delay": 120,
        "delayExtra": 30,
        "maxMBps": 100
    },
    "enableAudit": true,
    "auditPath": "/var/log/overlaybd-audit.log"
}
```

| Field               | Description                                                                                           |
| ---                 | ---                                                                                                   |
| logLevel            | DEBUG 0, INFO  1, WARN  2, ERROR 3                                                                    |
| ioEngine            | IO engine used to open local files: psync 0, libaio 1, posix aio 2.                                   |
| logPath             | The path for log file, `/var/log/overlaybd.log` is the default value.                                 |
| registryCacheDir    | The cache directory for remote image data.                                                            |
| registryCacheSizeGB | The max size of cache, in GB.                                                                         |
| cacheType           | Cache type used, `file` and `ocf` are supported, `file` is the default.                               |
| credentialFilePath  | The credential used for fetching images on registry. `/opt/overlaybd/cred.json` is the default value. |
| download.enable     | Whether background downloading is enabled or not.                                                     |
| download.delay      | The seconds waiting to start downloading task after the overlaybd device launched.                    |
| download.delayExtra | A random extra delay is attached to delay, avoiding too many tasks started at the same time.          |
| download.maxMBps    | The speed limit in MB/s for a downloading task.                                                       |
| enableAudit         | Enable audit or not.                                                                                  |
| auditPath           | The path for audit file, `/var/log/overlaybd-audit.log` is the default value.                         |

> NOTE: `download` is the config for background downloading. After an overlaybd device is lauched, a background task will be running to fetch the whole blobs into local directories. After downloading, I/O requests are directed to local files. Unlike other options, download config is reloaded when a device launching.

### credential config

Here is an example of credential file described by `credentialFilePath` field.

```json
{
  "auths": {
    "hub.docker.com": {
      "username": "username",
      "password": "password"
    },
    "hub.docker.com/hello/world": {
      "auth": "dXNlcm5hbWU6cGFzc3dvcmQK"
    }
  }
}
```

Credentials are reloaded when authentication is required.
Credentials have to be updated before expiration if temporary credential is used, otherwise overlaybd keeps reloading until a valid credential is set.

> **Important**: The corresponding credential has to be set before launching devices, if the registry is not public.

## Usage

### Use with containerd

Please install overlaybd and refer to  [Accelerated Container Image](https://github.com/containerd/accelerated-container-image). Overlaybd is well integrated with containerd and easy to use.

### Standalone Usage

For other scenarios, users can use overlaybd manually. Overlaybd works as a backing store of TCMU, so users can run overlaybd image by interacting with configfs.

#### Config file
A config file is required to describe an overlaybd image, only local image and registry image are supported. Here is a sample json config file:
```json
{
      "repoBlobUrl": "https://obd.cr.aliyuncs.com/v2/overlaybd/sample/blobs",
      "lowers" : [
          {
              "file" : "/opt/overlaybd/layer0"
          },
          {
              "dir": "/var/lib/containerd/root/io.containerd.snapshotter.v1.overlayfs/snapshots/1000",
              "digest": "sha256:e3b0d67cfa3a37dfed187badc7766e3db64d492c4db2dc4260997b41af1b28f3",
              "size": 43446424
          }
      ],
      "resultFile": "/home/overlaybd/1/result"
}
```
| Field               | Description |
| ---                 | ---         |
| repoBlobUrl         | the url of the repository blobs of the remote image. It is required for a registry image. |
| lowers              | a list describing the lower layers of the image in bottom-upper order. |
| file                | it means the corresponding layer is a local file. if a local file is used, other options are not needed. |
| dir                 | it means the corresponding layer will be stored in this directory after downloading. |
| digest and size     | the digest and size of a remote layer. It is required for a remote layer. |
| resultFile          | the file for saving the failure reasons. If a device is successfully lauched, success is writen into the file, otherwise, the failure s reported by this file. |


#### Start up

Here is an example to start up an overlaybd image.
First, create the overlaybd tcmu device.

``` bash
mkdir -p /sys/kernel/config/target/core/user_1/vol1
echo -n dev_config=overlaybd//root/config.v1.json > /sys/kernel/config/target/core/user_1/vol1/control
echo -n 1 > /sys/kernel/config/target/core/user_1/vol1/enable
```
Then, create a tcm loop device.
```bash
mkdir -p /sys/kernel/config/target/loopback/naa.123456789abcdef/tpgt_1/lun/lun_0
echo -n "naa.123456789abcdef" > /sys/kernel/config/target/loopback/naa.123456789abcdef/tpgt_1/nexus
ln -s /sys/kernel/config/target/core/user_1/vol1 /sys/kernel/config/target/loopback/naa.123456789abcdef/tpgt_1/lun/lun_0/vol1
```
Then a block device `/dev/sdX` is generated, overlaybd image can be used locally. Furthermore, overlaybd device can be used on remote hosts by iscsi.

#### Clean up
Just remove the files and directories in configfs in reverse order.

#### Writable layer
Overlaybd provides a log-structured writable layer and a sprase-file writable layer. Log-structured layer is append only and converts all writes into sequential writes so that the image build/convert process is usually faster. Sparse-file writable layer is more suitable for container rutime.

Use `overlaybd-create` to create a writable layer.
```bash
  /opt/overlaybd/bin/overlaybd-create ${data_file} ${index_file} ${virtual size}
```
use `-s` to for creating sparse-file writable layer.
The upper option in overlaybd config file must be set to use a writable layer. Only one writable layer is avialable and it always workes as the top layer.Example:
```json
{
    "repoBlobUrl": ...,
    "lowers" : [
        ...
    ],
    "upper": {
        "index": "${index_file}",
        "data": "${data_file}"
    },
    "resultFile": "/home/overlaybd/1/result"
}
```
If upper is set, the overlaybd device is launched as a writable device. The differences produced by data writing are stored in the index and data files ofupper.

After writing data and destroying the device, `overlaybd-commit` command is required to excute to commit the layer into a read-only layer and can be used asa lower layer later.
```bash
/opt/overlaybd/bin/overlaybd-commit ${data_file} ${index_file} ${commit_file}
```
At last, compression may be needed.
```bash
/opt/overlaybd/bin/overlaybd-zfile ${commit_file} ${zfile}
```
The zfile can be used as lower layer with online decompression.


## Kernel module

[DADI_kmod](https://github.com/data-accelerator/dadi-kernel-mod) is a kernel module of overlaybd. It can make local overlaybd-format files as a loop device or device-mapper.

## Contributing

Welcome to contribute! [CONTRIBUTING](CONTRIBUTING.md)

## Licenses

Overlaybd is released under the Apache License, Version 2.0.
