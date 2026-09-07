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

**注意**：新的 upper 层必须与旧的 upper 层不同。
