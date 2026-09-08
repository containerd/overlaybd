# 独立使用

[English](standalone-usage.md)

除了与 containerd 集成外，overlaybd 也可以手动驱动。一个 overlaybd 镜像通过以下两种内核后端之一暴露为虚拟块设备：

- **TCMU** —— overlaybd 作为 [TCMU](https://www.kernel.org/doc/Documentation/target/tcmu-design.txt) 的 backing store（后备存储）工作，通过与 `configfs` 交互来运行镜像。绝大多数 Linux 发行版均可用（加载 `target_core_user` 模块）。
- **UBLK**（Linux v6.0+）—— overlaybd 通过 [ublk](https://docs.kernel.org/block/ublk.html) 将镜像直接暴露为 `/dev/ublkbN`，无需经过 TCMU/SCSI（加载 `ublk_drv` 模块）。

两种后端使用相同的[配置文件](#配置文件)，并支持相同的[可写层](#可写层)。

## 配置文件

需要一个配置文件来描述一个 overlaybd 镜像，仅支持本地镜像和 registry 镜像。以下是一个示例 json 配置文件：
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
| 字段               | 说明 |
| ---                 | ---         |
| repoBlobUrl         | 远程镜像的仓库 blobs 的 url。对于 registry 镜像是必填的。 |
| lowers              | 一个列表，按自底向上（bottom-upper）的顺序描述镜像的各下层（lower layer）。 |
| file                | 表示对应的层是一个本地文件。如果使用本地文件，则无需其他选项。 |
| dir                 | 表示对应的层在下载后将存储在此目录中。 |
| digest and size     | 远程层的摘要（digest）和大小。对于远程层是必填的。 |
| resultFile          | 用于保存失败原因的文件。如果设备成功启动，则 success 会被写入该文件，否则失败信息通过该文件报告。 |

## TCMU 后端

### 启动

以下是启动一个 overlaybd 镜像的示例。
首先，创建 overlaybd tcmu 设备。

``` bash
mkdir -p /sys/kernel/config/target/core/user_1/vol1
echo -n dev_config=overlaybd//root/config.v1.json > /sys/kernel/config/target/core/user_1/vol1/control
echo -n 1 > /sys/kernel/config/target/core/user_1/vol1/enable
```
然后，创建一个 tcm loop 设备。
```bash
mkdir -p /sys/kernel/config/target/loopback/naa.123456789abcdef/tpgt_1/lun/lun_0
echo -n "naa.123456789abcdef" > /sys/kernel/config/target/loopback/naa.123456789abcdef/tpgt_1/nexus
ln -s /sys/kernel/config/target/core/user_1/vol1 /sys/kernel/config/target/loopback/naa.123456789abcdef/tpgt_1/lun/lun_0/vol1
```
随后会生成一个块设备 `/dev/sdX`，overlaybd 镜像即可在本地使用。此外，overlaybd 设备还可以通过 iscsi 在远程主机上使用。

### 清理
只需按相反顺序移除 configfs 中的文件和目录即可。

## UBLK 后端

运行 ublk 后端需要带有 `ublk_drv` 驱动的内核（mainline >= 6.0，或已回合（backport）ublk 的发行版内核）；用 `modprobe ublk_drv` 加载它（参见 [系统要求](../README_zh.md#系统要求)）。

### overlaybd-ublk（一个进程一个设备）

```bash
sudo overlaybd-ublk add --config /path/to/config.v1.json   # 就绪时会打印 /dev/ublkbN
sudo overlaybd-ublk list
sudo overlaybd-ublk del -n 0
```

与 `overlaybd-tcmu` 不同，一个 `overlaybd-ublk` 进程只服务一个设备；杀掉该守护进程即移除该设备。每个设备都可以通过 `add --log-path ...` 写入自己的日志文件（在运行多个设备时推荐这样做；否则各守护进程共享全局日志文件，并以 `ublk-<pid>` 标签区分）。

### overlaybd-ublkd（一个进程多个设备）

若要在单个进程中托管多个设备，还有 `overlaybd-ublkd`，这是一个集中式守护进程，共享单个 ImageService（因而共享一棵缓存树，位于 `--cache-dir` 下的 `<base>/daemon/`，由排他锁保护）。它的控制 API 是通过仅限 root 访问的 unix socket 提供的 HTTP，可用 curl 调试：

```bash
sudo overlaybd-ublkd &        # 或：systemctl start overlaybd-ublkd
SOCK=/var/run/overlaybd-ublk/ublkd.sock
curl --unix-socket $SOCK -X POST -d '{"config":"/path/config.v1.json"}' http://d/v1/add
curl --unix-socket $SOCK http://d/v1/list
curl --unix-socket $SOCK -X POST -d '{"dev_id":0}' http://d/v1/del
curl --unix-socket $SOCK -X POST -d '{"dev_id":0,"size_gb":50,"resize_fs":true}' http://d/v1/resize
curl --unix-socket $SOCK -X POST http://d/v1/shutdown   # 停止所有设备
```

`add` 在设备可用时返回；`del` 在完全拆除后返回；在线 `resize`（仅扩容、可写镜像）还需要带有 `UBLK_F_UPDATE_SIZE` 的内核（mainline >= 6.11）。可写镜像在守护进程和 CLI 进程之间以排他方式挂载；由守护进程持有的设备必须通过其 API 删除（CLI 的 `del` 会拒绝这些设备）。systemd 单元模板安装在 /opt/overlaybd/overlaybd-ublkd.service。

### 注意事项

* 在 `del` 之前（或停止守护进程之前）务必先卸载 `/dev/ublkbN` 上的文件系统：守护进程负责该设备的 IO，因此在文件系统仍挂载时将其拆除会中止正在进行（in-flight）的日志（journal）写入。
* 缓存始终按设备隔离：每个守护进程使用 `/opt/overlaybd/ublk_cache/<image-key>/<instance>/`（可通过 `add --cache-dir ...` 覆盖基础目录），并由排他锁保护——文件缓存的加锁仅在进程内有效，因此各守护进程绝不能共享同一缓存目录。多次挂载同一只读镜像会自动获得编号的实例（也可用 `--instance-id` 固定其一）；重新挂载某镜像会复用其热缓存（warm cache）；为避免破坏可写镜像的上层（upper layer），两次挂载同一可写镜像会被拒绝。运行多个设备时推荐为每个设备指定日志文件（`add --log-path ...`）。
* 运行时已在回合了 ublk 的 5.10 内核上验证（Alinux 5.10.134）；在 mainline 6.x 内核上的完整回归测试仍待进行。已知的回合内核注意事项：其上 DISCARD 不可用，且 `del` 耗时约 2s 而非毫秒级。

## 可写层

Overlaybd 提供了一种日志结构（log-structured）可写层和一种稀疏文件（sparse-file）可写层。日志结构层是只追加（append only）的，它将所有写入转换为顺序写入，因此镜像构建/转换过程通常更快。稀疏文件可写层更适合容器运行时。

使用 `overlaybd-create` 创建一个可写层。
```bash
  /opt/overlaybd/bin/overlaybd-create ${data_file} ${index_file} ${virtual size}
```
使用 `-s` 创建稀疏文件可写层。
要使用可写层，必须在 overlaybd 配置文件中设置 upper 选项。只有一个可写层可用，且它始终作为最顶层工作。示例：
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
如果设置了 upper，overlaybd 设备将作为可写设备启动。数据写入产生的差异会存储在 upper 的 index 和 data 文件中。

在写入数据并销毁设备之后，需要执行 `overlaybd-commit` 命令，将该层提交（commit）为一个只读层，之后便可作为下层（lower layer）使用。
```bash
/opt/overlaybd/bin/overlaybd-commit ${data_file} ${index_file} ${commit_file}
```
最后，可能还需要压缩。
```bash
/opt/overlaybd/bin/overlaybd-zfile ${commit_file} ${zfile}
```
生成的 zfile 可作为下层使用，并支持在线解压。
