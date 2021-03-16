# Overlaybd

## Accelerated Container Image

[Accelerated Container Image](https://github.com/alibaba/accelerated-container-image) is an open-source implementation of paper ["DADI: Block-Level Image Service for Agile and Elastic Application Deployment. USENIX ATC'20"](https://www.usenix.org/conference/atc20/presentation/li-huiba). 
It is a solution of remote container image by supporting fetching image data on-demand without downloading and unpacking the whole image before a container running.

At the heart of the acceleration is overlaybd, which provides a merged view of a sequence of block-based layers as an block device.
This repository is a component of Accelerated Container Image, provides an implementation of overlaybd and as a third-party backing-store of tgt, which is an user space iSCSI target framework.

The rest of this document will show you how to setup overlaybd.

## Setup

### System Requirements

Overlaybd provides virtual block devices through iSCSI protocol and tgt, so an iSCSI environment is required.

* [The Linux target framework (tgt)](https://github.com/fujita/tgt)
  * CentOS/Fedora: `sudo yum install scsi-target-utils` (epel maybe required)
  * Debian/Ubuntu: `sudo apt install tgt`

* iscsi-initiator-utils
  * CentOS/Fedora: `sudo yum install iscsi-initiator-utils`
  * Debian/Ubuntu: `sudo apt install open-iscsi`

### Install From RPM/DEB

You may download our RPM/DEB packages form [Release](https://github.com/alibaba/overlaybd/releases) and install.

### Build From Source

#### Requirements

To build overlaybd from source code, the following dependencies are required:

* CMake >= 3.8+

* gcc/g++ >= 7+

* Libaio, libcurl and openssl runtime and development libraries.
  * CentOS/Fedora: `sudo yum install libaio-devel libcurl-devel openssl-devel`
  * Debian/Ubuntu: `sudo apt install libcurl4-openssl-dev libssl-dev libaio-dev`

#### Build

You need git to checkout the source code:

```bash
git clone https://github.com/alibaba/overlaybd.git
```

The whole project is managed by CMake.

```bash
cd overlaybd

mkdir build
cd build
cmake ..
make -j
sudo make install

# restart tgt service to reload backing-store
sudo systemctl restart tgtd
```

A `liboverlaybd.so` file is installed to `/usr/lib{64}/tgt/backing-store`, tgtd service has to be restarted to load this dynamic library as a backing-store module.
Command-line tools are installed to `/opt/overlaybd/bin/`.

During compilation, some third-party dependency libraries will be automatically downloaded, see `CMake/external<lib_name>.cmake`. If you are having problems to download, you could manually prepare these libs under `external/<lib_name>/src/`, see CMake [doc](https://cmake.org/cmake/help/latest/module/ExternalProject.html).

## Configuration

### overlaybd config
Default configure file `tgt-overlaybd.json` is installed to `/etc/overlaybd/`.

```json
{
    "logLevel": 1,
    "registryCacheDir": "/opt/overlaybd/registry_cache",
    "registryCacheSizeGB": 1,
    "credentialFilePath": "/opt/overlaybd/cred.json",
    "ioEngine": 1,
    "download": {
        "enable": true,
        "delay": 120,
        "delayExtra": 30,
        "maxMBps": 100
    }
}
```

| Field               | Description                                                                                           |
| ---                 | ---                                                                                                   |
| logLevel            | DEBUG 0, INFO  1, WARN  2, ERROR 3                                                                    |
| ioEngine            | IO engine used to open local files: psync 0, libaio 1, posix aio 2.                                             |
| registryCacheDir    | The cache directory for remote image data.                                                            |
| registryCacheSizeGB | The max size of cache, in GB.                                                                         |
| credentialFilePath  | The credential used for fetching images on registry. `/opt/overlaybd/cred.json` is the default value. |
| download.enable     | Whether background downloading is enabled or not.                                                     |
| download.delay      | The seconds waiting to start downloading task after the overlaybd device launched.                    |
| download.delayExtra | A random extra delay is attached to delay, avoiding too many tasks started at the same time.          |
| download.maxMBps    | The speed limit in MB/s for a downloading task.

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

> **Important**: The corresponding credential has to be set before launching devices, if your image registry requires authentication. Otherwise overlaybd will keep on reloading until a valid credential is set.

For the convenience of testing, we provided a public registry on Aliyun ACR, see later examples.

## What's next?

Now we have finished the setup of overlaybd, let's go back to [Accelerated Container Image](https://github.com/alibaba/accelerated-container-image) repo and start to run our first accelerated container.

## Licenses

Overlaybd is released under the General Public License, Version 2.0.
