# OverlayBD

## Accelerated Container Image

[Accelerated Container Image](https://github.com/alibaba/accelerated-container-image) is an open-source implementation of paper ["DADI: Block-Level Image Service for Agile and Elastic Application Deployment. USENIX ATC'20"](https://www.usenix.org/conference/atc20/presentation/li-huiba). 
It is a solution of remote container image by supporting fetching image data on-demand without downloading and unpacking the whole image before a container running.

At the heart of the acceleration is OverlayBD, which provides a merged view of a sequence of block-based layers as an block device. 
This repository is a component of Accelerated Container Image, provides an implementation of OverlayBD and as a third-party backing-store of tgt, which is an user space iSCSI target framework.

## Getting Started
### Building
#### Requirements

To build OverlayBD, the following dependencies are required:

* CMake >= 3.8+

* gcc/g++ >= 7+

* Libaio, libcurl and openssl runtime and development libraries.
  * CentOS/Fedora: `sudo yum install libaio-devel libcurl-devel openssl-devel`
  * Debian/Ubuntu: `sudo apt install libcurl4-openssl-dev libssl-dev libaio-dev`

* [The Linux target framework (tgt)](https://github.com/fujita/tgt)
  * CentOS/Fedora: `yum install scsi-target-utils`, epel maybe required.
  * Debian/Ubuntu: `apt install tgt`

* iscsi-initiator-utils
  * CentOS/Fedora: `sudo yum install iscsi-initiator-utils`
  * Debian/Ubuntu: `sudo apt install open-iscsi`


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
make
sudo make install

# restart tgt service to reload backing-store
sudo systemctl restart tgtd
```

A `liboverlaybd.so` file is installed to `/usr/lib{64}/tgt/backing-store`,tgtd service has to be restarted to load this dynamic library as a backing-store module.
OverlayBD related command-line tools are installed to `/opt/overlaybd/bin/`.

### Configure

The default configuration files are installed after `make install`.

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
| download.delay      | The seconds waiting to start downloading task after the OverlayBD device launched.                    |
| download.delayExtra | A random extra delay is attached to delay, avoiding too many tasks started at the same time.          |
| download.maxMBps    | The speed limit in MB/s for a downloading task.

> NOTE: `download` is the config for background downloading. After an OverlayBD device is lauched, a background task will be running to fetch the whole blobs into local directories. After downloading, I/O requests are directed to local files. Different to other options, download config is reloaded when a device lauching.

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
Credentials have to be updated before expiration if temporary credential is used, otherwise OverlayBD keeps reloading until a valid credential is set.

> **Important**: The corresponding credential has to be set before launching devices.

### Usage

OverlayBD is working together with overlaybd-snapshotter and ctr plugin.
See [EXAMPLES](https://github.com/alibaba/accelerated-container-image/blob/main/docs/EXAMPLES.md)

## Licenses

* OverlayBD is released under the General Public License, Version 2.0.
