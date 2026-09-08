# Standalone Usage

[简体中文](standalone-usage_zh.md)

Besides the containerd integration, overlaybd can be driven manually. An overlaybd
image is exposed as a virtual block device through one of two kernel backends:

- **TCMU** — overlaybd works as a backing store of
  [TCMU](https://www.kernel.org/doc/Documentation/target/tcmu-design.txt); you run
  an image by interacting with `configfs`. Available on most Linux distributions
  (load the `target_core_user` module).
- **UBLK** (Linux v6.0+) — overlaybd exposes the image directly as `/dev/ublkbN`
  through [ublk](https://docs.kernel.org/block/ublk.html), without going through
  TCMU/SCSI (load the `ublk_drv` module).

Both backends consume the same [config file](#config-file) and support the same
[writable layer](#writable-layer).

## Config file

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

## TCMU backend

### Start up

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

### Clean up
Just remove the files and directories in configfs in reverse order.

## UBLK backend

Running the ublk backend requires a kernel with the `ublk_drv` driver (mainline
>= 6.0, or a distro kernel with ublk backported); load it with
`modprobe ublk_drv` (see [System Requirements](../README.md#system-requirements)).

### overlaybd-ublk (one device per process)

```bash
sudo overlaybd-ublk add --config /path/to/config.v1.json   # prints /dev/ublkbN when ready
sudo overlaybd-ublk list
sudo overlaybd-ublk del -n 0
```

Unlike `overlaybd-tcmu`, one `overlaybd-ublk` process serves exactly one
device; killing the daemon removes the device. Each device can write to its
own log file via `add --log-path ...` (recommended when running multiple
devices; daemons otherwise share the global log file and are distinguished
by a `ublk-<pid>` tag).

### overlaybd-ublkd (many devices in one process)

For hosting many devices in one process there is also `overlaybd-ublkd`, a
centralized daemon sharing a single ImageService (and thus one cache tree,
`<base>/daemon/` under `--cache-dir`, guarded by an exclusive lock). Its
control API is HTTP over a root-only unix socket, debuggable with curl:

```bash
sudo overlaybd-ublkd &        # or: systemctl start overlaybd-ublkd
SOCK=/var/run/overlaybd-ublk/ublkd.sock
curl --unix-socket $SOCK -X POST -d '{"config":"/path/config.v1.json"}' http://d/v1/add
curl --unix-socket $SOCK http://d/v1/list
curl --unix-socket $SOCK -X POST -d '{"dev_id":0}' http://d/v1/del
curl --unix-socket $SOCK -X POST -d '{"dev_id":0,"size_gb":50,"resize_fs":true}' http://d/v1/resize
curl --unix-socket $SOCK -X POST http://d/v1/shutdown   # stops all devices
```

`add` replies once the device is usable; `del` replies after full teardown;
online `resize` (grow only, writable images) additionally needs a kernel
with `UBLK_F_UPDATE_SIZE` (mainline >= 6.11). Writable images are mounted
exclusively across the daemon and CLI processes; devices owned by the
daemon must be deleted through its API (the CLI `del` refuses them). A
systemd unit template is installed at /opt/overlaybd/overlaybd-ublkd.service.

### Notes

* Always unmount filesystems on `/dev/ublkbN` before `del` (or before stopping
  the daemon): the daemon serves the device's IO, so tearing it down with a
  mounted filesystem aborts in-flight journal writes.
* Caches are always isolated per device: each daemon uses
  `/opt/overlaybd/ublk_cache/<image-key>/<instance>/` (override the base
  directory with `add --cache-dir ...`), guarded by an exclusive lock -- the
  file cache's locking is in-process only, so daemons must never share a
  cache directory. Mounting the same read-only image multiple times gets
  automatically numbered instances (or pin one with `--instance-id`);
  remounting an image reuses its warm cache; mounting a writable image twice
  is rejected to protect its upper layer. A per-device log file
  (`add --log-path ...`) is recommended when running multiple devices.
* Runtime verified on a 5.10 kernel with ublk backported (Alinux
  5.10.134); a full regression on mainline 6.x kernels is still pending.
  Known backport-kernel caveats: DISCARD is unavailable there, and `del`
  takes ~2s instead of milliseconds.

## Writable layer

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
