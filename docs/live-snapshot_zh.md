# 实时快照（Live Snapshot）

[English](live-snapshot.md)

Overlaybd 支持在不停止设备的情况下创建实时快照。该特性允许你捕获可写层的当前状态，并在其之上叠加一个新的可写层。

## 设备 ID（Device ID）

要使用实时快照特性，你需要在创建 overlaybd 设备时指定一个设备 ID。设备 ID 以分号分隔符追加到 config 路径之后：

```bash
echo -n dev_config=overlaybd//root/config.v1.json;123 > /sys/kernel/config/target/core/user_1/vol1/control
```

## 启用 API 服务

在你的 `overlaybd.json` 中添加以下内容：

```json
"serviceConfig": {
    "enable": true,
    "address": "http://127.0.0.1:9862"
}
```

## 创建快照

向 `/snapshot` 端点发送一个 HTTP POST 请求：

```bash
curl -X POST "http://127.0.0.1:9862/snapshot?dev_id=123&config=/path/to/new_config.json"
```

响应将为 JSON 格式：

```json
{
    "success": true,
    "message": "Snapshot created successfully"
}
```

## 新配置格式

新的配置文件应将当前的 upper 层作为最后一个 lower 层包含进来：

```json
{
    "lowers": [
        {
            "file": "/opt/overlaybd/layer0"
        },
        {
            "file": "/path/to/current_upper_data.lsmt"
        }
    ],
    "upper": {
        "index": "/path/to/new_upper_index.lsmt",
        "data": "/path/to/new_upper_data.lsmt"
    }
}
```

lowers 由旧的 lowers 追加一层构成：第 N+1 个 lower 是旧 upper 的 data 文件经 seal 得到，其 `file` 等于当前 `upper.data` 时原地 seal；为其它绝对路径时，seal 后把 data 文件 rename 到该路径，要求目标不存在、与 data 文件同文件系统，父目录缺失时自动创建。

前 N 项与旧的 lowers 逐字段一致，upper 必须与旧的 upper 不同。旧 upper 的 `upper.index` 快照后不再被引用，可由调用方清理。

## 失败语义与崩溃恢复

提交点是更新 config：之前失败返回 -1、清理事务文件，层结构不变，可立即重试；之后只剩内存操作。描述已提交层结构的请求直接返回成功，调用方传入的 config 不会被消费。

config 旁有两个事务文件：`.snapshot`（待更新的 config 字节，存在即未提交）与 `.snapshot.undo`（改名目标，仅改名时存在）。未提交时先按 `.undo` 把 sealed 层移回原路径，再清理两个文件；已提交时只清理残留的 `.undo`。恢复只覆盖进程崩溃（kill、异常退出），主机掉电不在保证范围内。

seal 失败处理：seal 只追加写，data 尾部仅留下不可达的 index 与 trailer，header 未变，重启后的设备仍可打开，因此不需要回滚。稀疏可写层例外：其映射靠扫描 data 文件重建，因此打开时需要按 `virtual_size` 截断。

## 限制

- upper 为 warpfile（`upper.target`）时，不支持快照。
- upper 为 hybrid 或 sparse 时，写路径不持快照使用的写锁，快照期间需调用方暂停写入。
- 同一设备的快照串行执行；快照期间设备的写路径被占用。
