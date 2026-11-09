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

#### Import a local blob into the managed cache

After publishing an immutable blob to a registry, a local producer can seed the daemon's existing file cache without downloading the same bytes again:

```bash
BLOB=/absolute/path/to/finalized-blob
DIGEST=sha256:$(sha256sum "$BLOB" | cut -d ' ' -f 1)
SIZE=$(stat -c %s "$BLOB")
curl --unix-socket "$SOCK" -X POST -H 'Content-Type: application/json' \
  -d "{\"path\":\"$BLOB\",\"url\":\"https://registry.example/v2/repository/blobs/$DIGEST\",\"digest\":\"$DIGEST\",\"size\":$SIZE}" \
  http://d/v1/cache/import
```

The source must be a regular file visible in the daemon's mount namespace; the final path component cannot be a symlink. Keep its contents immutable until the request completes. The daemon verifies the positive size and SHA-256 before writing any cache bytes, then verifies each reread chunk against the first pass before publishing it. In-place mutation cannot publish unchecked bytes; an error may leave an already verified prefix in cache. The URL must be an HTTP(S) registry blob URL ending in `/blobs/<digest>` and should match the URL used by subsequent image reads. This root-only local operation does not publish the blob or authenticate to the registry; publish durable content before importing it.

The endpoint returns `200` on completion, `400` for invalid requests or local content, `409` when the managed file cache is unavailable or another cache type is configured, `413` for blobs above 64 GiB, `429` while another import is active, and `503` if cache population cannot complete. Both `cacheConfig.cacheType: "file"` and the legacy top-level `cacheType: "file"` are supported; an omitted or empty nested type falls back to the top-level type, whose default is `file`.

Imports are synchronous, with one active import per ImageService and no waiting queue. Disconnecting the HTTP client does not cancel an admitted import; wait for completion or retry with backoff on `429`. Local file reads are synchronous and yield between 256 KiB chunks, so slow local storage can still delay daemon work. Integrity proofs use at most 8 MiB plus a 256 KiB data buffer per active import. A `200` means all writes were accepted, not that bytes are pinned, durable across crashes, or guaranteed resident after return.

Import is an optional optimization: failures, partial imports, and later eviction retain the normal source-read path. Producers may remove their staging files after the request finishes. Cache imports use the same capacity, LRU eviction, allocation accounting, and free-space protection as downloaded blobs; they do not pin entries or create a separate retained copy. The ingestion API belongs to ImageService and is available independently of the ublk HTTP adapter. Registry publication, readiness checks, retries, staging cleanup and Fast Sandbox orchestration remain the producer's responsibility. Existing active device and mount lifecycles are unchanged.

#### Local cache first with P2P

To use imported blobs while P2P is enabled, opt into `p2pConfig.cacheFirst` in the daemon's service configuration:

```json
{
  "cacheConfig": {"cacheType": "file"},
  "p2pConfig": {
    "enable": true,
    "address": "http://127.0.0.1:9731/accelerator",
    "cacheFirst": true
  }
}
```

In this mode, image reads always enter the managed file cache. Cached ranges are served locally; missing or evicted ranges are fetched through RegistryFS using the selected P2P accelerator and refill the same cache. Imports may run before the first image is created. Selecting or rechecking acceleration during image creation does not bypass the cache. If the accelerator availability check fails, RegistryFS uses the registry directly while keeping the local cache in front; this does not add per-request failover for accelerator failures after selection.

`cacheFirst` defaults to `false`, preserving the existing P2P cache-bypass behavior, and requires the effective cache type to be `file`. When P2P is enabled without `cacheFirst`, imports return `409` even before the first image selects a read path: a later successful P2P probe could otherwise bypass the imported data. Set `cacheFirst: true` to combine P2P with imports, or disable P2P to use the ordinary file-cache path. Cache-first mode also caches data downloaded through P2P, so a P2P service with its own disk cache may retain a second local copy. Existing cache capacity and eviction policies still apply.

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
