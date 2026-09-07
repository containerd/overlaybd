# Overlaybd

[English](README.md)

![logo](https://github.com/containerd/overlaybd/blob/main/docs/assets/overlaybd_logo.svg)

Overlaybd（overlay block device，覆盖块设备）是一种全新的块级分层镜像格式，为容器、安全容器设计，并适用于虚拟机。它是论文 [DADI: Block-Level Image Service for Agile and Elastic Application Deployment. USENIX ATC'20"](https://www.usenix.org/conference/atc20/presentation/li-huiba) 的开源实现。

<img src="https://github.com/containerd/overlaybd/blob/main/docs/assets/Scaling_up.jpg" width="400px">

[Scaling up Without Slowing Down: Accelerating Pod Start Time. KubeCon+CloudNativeCon Europe 2024](https://youtu.be/RJ6Lt9bVNTw)

Overlaybd 基于 [PhotonLibOS](https://github.com/alibaba/PhotonLibOS) 构建，后者是一个高效的 LibOS 框架。

Overlaybd 有 2 个核心组件：
* **Overlaybd**
  是一种基于块设备的镜像格式，将一系列块级层（layer）合并后以虚拟块设备的形式提供统一视图。其 LBA 查找算法采用线性化 B+ 树并结合 AVX-512 优化性能，可将查找速度最高提升 10 倍。[查找性能](https://github.com/containerd/overlaybd/blob/main/docs/lsmt_lookup.md)

* **Zfile**
  是一种支持可随机寻址（seekable）在线解压的压缩文件格式。

本仓库是 overlaybd 基于 [TCMU](https://www.kernel.org/doc/Documentation/target/tcmu-design.txt) 的实现。

Overlaybd 可作为 [Accelerated Container Image](https://github.com/containerd/accelerated-container-image)（加速容器镜像）的存储后端；该方案通过按需拉取镜像数据实现远程容器镜像，无需在容器启动前下载并解压整个镜像。

得益于块设备的通用性，overlaybd 也是一种适用于绝大多数运行时的镜像格式，包括 qemu/kvm 以及任何支持 block 或 scsi api 的运行时。

Overlaybd 是 containerd 的一个 __非核心（non-core）__ 子项目。

## 安装部署

### 系统要求

Overlaybd 通过 TCMU 提供虚拟块设备，因此需要 TCMU 内核模块。TCMU 已在 Linux 内核中实现，并被大多数 Linux 发行版支持。
对于 Linux v6.0+ 内核，支持通过 UBLK 创建 overlaybd 设备。

检查并加载 target_core_user 模块。

```bash
modprobe target_core_user ## TCMU
modprobe ublk_drv ## UBLK, linux v6.0+ requires
```

### 从 RPM/DEB 安装

你可以从 [Release](https://github.com/containerd/overlaybd/releases) 下载我们的 RPM/DEB 包并安装。

二进制文件会被安装到 `/opt/overlaybd/bin/`。

运行 `/opt/overlaybd/bin/overlaybd-tcmu`，日志存储在 `/var/log/overlaybd.log`。

最好将 `overlaybd-tcmu` 作为服务（service）运行，以便在意外崩溃后能够自动重启。

### 从源码构建

#### 依赖要求

从源码构建 overlaybd 需要以下依赖：

* CMake >= 3.14

* gcc/g++ >= 7

* Libaio、libcurl、libnl3、glib2 和 openssl 的运行时库及开发库。
  * CentOS 7/Fedora: `sudo yum install libaio-devel libcurl-devel openssl-devel libnl3-devel libzstd-static e2fsprogs-devel`
  * CentOS 8: `sudo yum install libaio-devel libcurl-devel openssl-devel libnl3-devel libzstd-devel e2fsprogs-devel`
  * Debian/Ubuntu: `sudo apt install libcurl4-openssl-dev libssl-dev libaio-dev libnl-3-dev libnl-genl-3-dev libgflags-dev libzstd-dev libext2fs-dev pkg-config automake libtool # libgtest-dev // 用于测试`
  * Mariner/AzureLinux: `sudo yum install libaio-devel libcurl-devel openssl-devel libnl3-devel e2fsprogs-devel glibc-devel libzstd-devel binutils ca-certificates-microsoft build-essential`

#### 构建

你需要用 git 检出源码：

```bash
git clone https://github.com/containerd/overlaybd.git
cd overlaybd
git submodule update --init
```

整个项目由 CMake 管理。二进制文件和资源文件将被安装到 `/opt/overlaybd/`。

```bash
mkdir build
cd build
cmake .. # -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=true -DBUILD_TESTING=true
make -j
sudo make install
```

考虑到某些 libcurl 和 libopenssl 存在 API 变更，如果希望构建一个确保兼容版本的 libcurl 和 openssl，并以静态库方式链接到可执行文件。

注意，构建 libcurl 和 openssl 依赖 `autoconf`、`automake` 和 `libtool`。

```bash
cmake -D BUILD_CURL_FROM_SOURCE=1 ..
```

如果你希望使用[原版 libext2fs](https://github.com/tytso/e2fsprogs) 而非我们[定制版的 libext2fs](https://github.com/data-accelerator/e2fsprogs)。

```bash
cmake -D ORIGIN_EXT2FS=1 ..
```

关于 `ORIGIN_EXT2FS` 的更多信息请参阅 [USERSPACE_CONVERTOR](https://github.com/containerd/accelerated-container-image/blob/main/docs/USERSPACE_CONVERTOR.md#libext2fs)。

如果你希望使用 DSA 硬件加速 CRC 计算。

```bash
cmake -D ENABLE_DSA=1 ..
```

如果你希望使用 avx512 加速 CRC 计算。

```bash
cmake -D ENABLE_ISAL=1 ..
```

如果你希望使用 QAT 加速压缩/解压。不过目前仅集成了解压部分。由于 LZ4 本身已是一种高效压缩算法，我们的测试表明，只有当压缩比显著超过某一阈值、且在 4KB 块大小和 256 批量大小时，QAT 才能优于 CPU。

```bash
cmake -D ENABLE_QAT=1 ..
```

更多信息请参阅 `overlaybd/src/overlaybd/zfile/README.md`。

#### 使用 UBLK 作为 overlaybd 块设备后端

如果你希望构建 ublk 前端（`overlaybd-ublk`），它将镜像暴露为 `/dev/ublkbN`，而无需经过 TCMU/SCSI。它仅在内核支持 ublk 时默认构建（通过 `ublk_drv` 模块或 `linux/ublk_cmd.h` uapi 头自动检测）；可用 `-D BUILD_UBLK_FRONTEND=on|off` 强制开启或关闭。构建它还额外需要 `autoconf`、`automake` 和 `libtool`（liburing 和 libublksrv 会自动从源码拉取并构建）。

```bash
cmake -D BUILD_UBLK_FRONTEND=on ..
```

运行 ublk 后端需要带有 `ublk_drv` 驱动的内核（mainline >= 6.0，或已回合（backport）ublk 的发行版内核）。关于如何通过 `overlaybd-ublk` / `overlaybd-ublkd` 运行镜像，参见[独立使用](docs/standalone-usage_zh.md)的 [UBLK 后端](docs/standalone-usage_zh.md#ublk-后端)一节。

最后，为 overlaybd-tcmu backstore 设置一个 systemd 服务。

```bash
sudo systemctl enable /opt/overlaybd/overlaybd-tcmu.service
sudo systemctl start overlaybd-tcmu
```

## 配置

### overlaybd 配置
默认配置文件 `overlaybd.json` 会被安装到 `/etc/overlaybd/`。

```json
{
    "logConfig": {
        "logLevel": 1,
        "logPath": "/var/log/overlaybd.log"
    },
    "cacheConfig": {
        "cacheType": "file",
        "cacheDir": "/opt/overlaybd/registry_cache",
        "cacheSizeGB": 4
    },
    "gzipCacheConfig": {
        "enable": true,
        "cacheDir": "/opt/overlaybd/gzip_cache",
        "cacheSizeGB": 4
    },
    "credentialConfig": {
        "mode": "file",
        "path": "/opt/overlaybd/cred.json"
    },
    "ioEngine": 0,
    "download": {
        "enable": true,
        "delay": 600,
        "delayExtra": 30,
        "maxMBps": 100
    },
    "p2pConfig": {
        "enable": false,
        "address": "localhost:19145/dadip2p"
    },
    "exporterConfig": {
        "enable": false,
        "uriPrefix": "/metrics",
        "port": 9863,
        "updateInterval": 60000000
    },
    "enableAudit": true,
    "auditPath": "/var/log/overlaybd-audit.log",
    "serviceConfig": {
        "enable": false,
        "address": "http://127.0.0.1:9862"
    }
}
```

| 字段               | 说明                                                                                           |
|---------------------|-------------------------------------------------------------------------------------------------------|
| logConfig.logLevel      | 日志文件的日志级别，0 - DEBUG，1 - INFO，2 - WARN，3 - ERROR                              |
| logConfig.logPath       | 日志文件路径，默认值为 `/var/log/overlaybd.log`。                             |
| logConfig.logSizeMB     | 日志文件大小上限，单位 MB，默认 `10`（10 MB）。                                      |
| logConfig.logRotateNum  | 日志文件轮转（rotate）数量，默认 `3`。                                                   |
| ioEngine                | 用于打开本地文件的 IO 引擎：psync 0，libaio 1，posix aio 2。                               |
| cacheConfig.cacheType   | 使用的缓存类型，支持 `file`、`ocf` 和 `download`。                                      |
| cacheConfig.cacheDir    | 远程镜像数据的缓存目录。                                                        |
| cacheConfig.cacheSizeGB | 缓存最大容量，单位 GB。                                                                     |
| cacheConfig.refillSize  | 从源回填（refill）的大小，单位字节，默认 `262144`（256 KB）。                               |
| gzipCacheConfig.enable      | 是否启用解压后的 gzip 文件缓存。                                       |
| gzipCacheConfig.cacheDir    | 解压后 gzip 数据的缓存目录。                                               |
| gzipCacheConfig.cacheSizeGB | 缓存最大容量，单位 GB。                                                                 |
| gzipCacheConfig.refillSize  | 从源回填的大小，单位字节，默认 `262144`（256 KB）。                           |
| credentialFilePath(legacy)  | 用于从 registry 拉取镜像的凭证。默认值为 `/opt/overlaybd/cred.json`。 |
| credentialConfig.mode       | 懒加载（lazy-loading）的认证模式。 <br> - `file` 表示从 `credentialConfig.path` 读取凭证。  <br> - `http` 表示向 `credentialConfig.path` 发送 http 请求 <br> - `https` 表示向 `credentialConfig.path` 发送 https 请求，可选客户端证书认证和 CA 固定（pinning） <br> - `uds` 表示发送与 `http` 模式相同的 http 请求，但通过位于 `credentialConfig.path` 的 Unix 域套接字（Unix-domain socket）传输 |
| credentialConfig.path       | 凭证文件路径或 url，由 `mode` 决定                                     |
| credentialConfig.client_cert_path | 可选。客户端证书文件路径（`https` 模式）。同一个 PEM 文件中可包含私钥。 |
| credentialConfig.client_key_path  | 可选。客户端私钥文件路径（`https` 模式）。仅当私钥与证书分离时才需要。 |
| credentialConfig.server_ca_path   | 可选。用于校验服务端的 CA 证书路径（`https` 模式）。若省略，则使用系统 CA bundle。设置后，将**仅**信任该 CA 文件。 |
| download.enable     | 是否启用后台下载。                                                     |
| download.delay      | overlaybd 设备启动后，等待多少秒再开始下载任务。                    |
| download.delayExtra | 在 delay 基础上附加一个随机额外延迟，避免过多任务同时启动。          |
| download.maxMBps    | 单个下载任务的限速，单位 MB/s。                                                       |
| download.blockSize  | 从源下载的块大小，单位字节，默认 `262144`（256 KB）。                           |
| p2pConfig.enable    | 是否启用 p2p 代理。                                                                  |
| p2pConfig.address   | p2p 下载的代理，格式为 `localhost:<P2PConfig.Port>/<P2PConfig.APIKey>`，取决于 dadip2p.yaml |
| exporterConfig.enable         | 是否创建用于展示 Prometheus 指标（metrics）的服务端。                                  |
| exporterConfig.uriPrefix      | 导出指标的 URI 前缀。                                                              |
| exporterConfig.port           | 用于展示指标的 http 服务端端口。                                                       |
| exporterConfig.updateInterval | 更新指标的时间间隔，单位微秒。                                            |
| enableAudit         | 是否启用审计（audit）。                                                                                  |
| enableThread        | 是否让 overlaybd 设备运行在独立线程中。注意 `cacheType` 应为 `ocf`。默认 `false`。 |
| auditPath           | 审计文件路径，默认值为 `/var/log/overlaybd-audit.log`。                         |
| registryFsVersion   | registry 客户端版本，'v1' 基于 libcurl，'v2' 基于 photon http。默认值为 'v2'。    |
| prefetchConfig.concurrency    | 重载 trace 时的预取（prefetch）并发度，默认 `16`                                   |
| certConfig.certFile | SSL/TLS 客户端证书文件路径                                                          |
| certConfig.keyFile  | SSL/TLS 客户端密钥文件路径                                                                  |
| userAgent  | 自定义 userAgent，用于标识 HTTP 请求。默认值为包版本，如 'overlaybd/1.1.14-6c449832'      |
| serviceConfig.enable    | 是否启用实时快照（live snapshot）API 服务，默认 `false`。                                      |
| serviceConfig.address   | API 服务监听地址，默认 `http://127.0.0.1:9862`。                             |


> 注意：`download` 是后台下载的配置。overlaybd 设备启动后，会有一个后台任务运行，将整个 blobs 拉取到本地目录。下载完成后，I/O 请求将被定向到本地文件。与其他选项不同，download 配置在设备启动时会被重新加载。

### 凭证配置

> **重要**：如果 registry 不是公开的，则必须在启动设备之前设置好相应的凭证。

凭证会在需要认证时被重新加载。如果使用临时凭证，则必须在过期前更新凭证，否则 overlaybd 会持续重新加载，直到设置了有效凭证为止。

Overlaybd 支持多种凭证模式。以下是一些 `credentialConfig` 字段示例。

- **file** 模式

  `credentialConfig.path` 应类似于 '.docker/config.json'，如下所示：
```json
#### /etc/overlaybd/config.json ####
{
  "logLevel": 1,
  "logPath": "/var/log/overlaybd.log",
  ...
  "credentialConfig": {
      "mode": "file",
      "path": "/opt/overlaybd/cred.json"
    },
  ...
}
#### /opt/overlaybd/cred.json ####
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

- **http** 模式

  `credentialConfig.path` 应为由开发者实现、能够响应凭证信息的服务端监听地址。

```json
#### /etc/overlaybd/config.json ####
{
  "logLevel": 1,
  "logPath": "/var/log/overlaybd.log",
  ...
  "credentialConfig": {
      "mode": "http",
      "path": "localhost:19876/auth"
    },
  ...
}
```
  overlaybd 会向该服务端发送带有 `remote_url` 的 http 请求，如下：
> GET "localhost:19876/auth?remote_url=https://hub.docker.com/v2/overlaybd/ubuntu/blobs/sha256:47e63559a8487efb55b2f1ccea9cfc04110a185c49785fdf1329d1ea462ce5f0"
  服务端响应应按如下格式：
```json
{
  "traceId": "${trace_id}"
  "success": true or false
  "data": {
    "auths": {
      "hub.docker.com": {
        "username": "username",
        "password": "password"
      }
    }
  }
}
```
我们在 `test/simple_auth_server.cpp` 中写了一个示例 http 服务端

- **https** 模式

  `credentialConfig.path` 应为一个 HTTPS 服务端监听地址。与 `http` 模式不同，path 中必须包含 `https://` 前缀（例如 `https://localhost:19876/auth`）。可选的 `client_cert_path`/`client_key_path` 字段启用客户端证书认证，`server_ca_path` 将信任固定到特定 CA。对于本地认证服务端，同时提供这三个字段可确保仅与该服务端进行安全通信（双向 TLS，mTLS）。

```json
#### /etc/overlaybd/config.json ####
{
  "logLevel": 1,
  "logPath": "/var/log/overlaybd.log",
  ...
  "credentialConfig": {
      "mode": "https",
      "path": "https://localhost:19876/auth",
      "client_cert_path": "/etc/overlaybd/client.crt",
      "client_key_path": "/etc/overlaybd/client.key",
      "server_ca_path": "/etc/overlaybd/ca.crt"
    },
  ...
}
```
  overlaybd 会通过 mTLS 向该服务端发送带有 `remote_url` 的 https 请求，如下：
> GET "https://localhost:19876/auth?remote_url=https://hub.docker.com/v2/overlaybd/ubuntu/blobs/sha256:47e63559a8487efb55b2f1ccea9cfc04110a185c49785fdf1329d1ea462ce5f0"
  服务端响应格式与 `http` 模式相同。

  三个 TLS 字段均为可选且可独立配置：
  - `client_cert_path` 设置客户端证书。如果该 PEM 文件同时包含私钥，则可省略 `client_key_path`。
  - `client_key_path` 设置客户端私钥。仅当私钥位于与证书分离的文件中时才需要。
  - 若省略 `server_ca_path`，则使用系统 CA bundle 校验服务端证书。设置 `server_ca_path` 后，将**仅**使用指定的 CA 文件——不再查询系统 CA bundle。

- **uds** 模式

  `credentialConfig.path` 应为 Unix 域套接字的文件系统路径。overlaybd 会拨号（dial）该套接字，并使用与 `http` 模式相同的 HTTP 请求/响应格式。

```json
#### /etc/overlaybd/config.json ####
{
  "logLevel": 1,
  "logPath": "/var/log/overlaybd.log",
  ...
  "credentialConfig": {
      "mode": "uds",
      "path": "/run/overlaybd/creds.sock"
    },
  ...
}
```
  overlaybd 会拨号该套接字并发送如下请求：
> GET "http://localhost/auth?remote_url=https://hub.docker.com/v2/overlaybd/ubuntu/blobs/sha256:47e63559a8487efb55b2f1ccea9cfc04110a185c49785fdf1329d1ea462ce5f0"

  这里的 HTTP host（`localhost`）只是一个占位符——请求始终通过配置的套接字拨号发送。服务端响应格式与 `http` 模式完全相同。

  安全模型：UDS 不可通过网络访问，并由套接字文件上的文件系统权限进行管控。请在辅助服务端（helper side）通过设置套接字的属主/属组以及非全局可读的权限模式（例如 `0600` 或 `0660`）来限制访问；overlaybd 不会在客户端强制实施这一点。


## 使用

### 与 containerd 一起使用

请安装 overlaybd 并参考 [Accelerated Container Image](https://github.com/containerd/accelerated-container-image)。Overlaybd 与 containerd 集成良好，易于使用。

### 独立使用

对于其他场景，用户可以手动驱动 overlaybd。一个 overlaybd 镜像通过两种内核后端之一暴露为虚拟块设备——**TCMU**（`configfs`，多数发行版）或 **UBLK**（`/dev/ublkbN`，Linux v6.0+），并可选地携带一个可写层。

配置文件格式、TCMU 与 UBLK 的启动/拆除流程，以及可写层（create / commit / zfile）命令，参见 [docs/standalone-usage_zh.md](docs/standalone-usage_zh.md)。

### 实时快照（Live Snapshot）

Overlaybd 可以在不停止设备的情况下创建实时快照：捕获可写层的当前状态，并在其之上叠加一个新的可写层。

设备 ID、API 服务配置以及 `/snapshot` 请求/响应细节，参见 [docs/live-snapshot_zh.md](docs/live-snapshot_zh.md)。

## 内核模块

[DADI_kmod](https://github.com/data-accelerator/dadi-kernel-mod) 是 overlaybd 的一个内核模块。它可以将本地的 overlaybd 格式文件作为 loop 设备或 device-mapper 使用。

## 贡献

欢迎贡献！[CONTRIBUTING](CONTRIBUTING.md)

## 许可证

Overlaybd 基于 Apache License 2.0 发布。
