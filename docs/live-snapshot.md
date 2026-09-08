# Live Snapshot

[简体中文](live-snapshot_zh.md)

Overlaybd supports creating live snapshots without stopping the device. This feature allows you to capture the current state of a writable layer and stack a new writable layer on top.

## Device ID

To use the live snapshot feature, you need to specify a device ID when creating the overlaybd device. The device ID is appended to the config path with a semicolon separator:

```bash
echo -n dev_config=overlaybd//root/config.v1.json;123 > /sys/kernel/config/target/core/user_1/vol1/control
```

## Enable API Service

Add the following to your `overlaybd.json`:

```json
"serviceConfig": {
    "enable": true,
    "address": "http://127.0.0.1:9862"
}
```

## Create Snapshot

Send an HTTP POST request to the `/snapshot` endpoint:

```bash
curl -X POST "http://127.0.0.1:9862/snapshot?dev_id=123&config=/path/to/new_config.json"
```

The response will be in JSON format:

```json
{
    "success": true,
    "message": "Snapshot created successfully"
}
```

## New Config Format

The new config file should include the current upper layer as the last lower layer:

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

**Note**: The new upper layer must be different from the old upper layer.
