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

`lowers` is the old list plus one entry: the (N+1)-th lower is the old upper data file, sealed. Where that layer ends up is decided by its `file`: the current `upper.data` keeps it in place, while any other absolute path moves it there as part of the snapshot. A move requires the target to be absent and on the same file system as the data file; missing parent directories are created.

The first N entries must match the old lowers exactly. The old upper's `upper.index` is not referenced after the snapshot and can be deleted by the caller.

## Failure Semantics and Crash Recovery

Updating the config is the commit point. A failure before it returns -1, removes the transaction files and leaves the layers untouched, so the request can be retried right away; after it, only in-memory operations remain. A request that describes the stack already committed returns success, and the caller's config file is left as is.

Two transaction files live next to the config: `.snapshot` holds the new config bytes, so its presence means the snapshot is uncommitted; `.snapshot.undo` holds the move target and exists only for a move. If the snapshot is uncommitted, the sealed layer is moved back as `.undo` records and both files are cleaned up; if it is committed, only a stale `.undo` is removed. Recovery covers process crashes only; a host power loss is out of scope.

A failed seal needs no rollback: it only appends, leaving an unreachable index and trailer at the end of the data file, and since the header is untouched the device still opens after a restart. Sparse writable layers are the exception: their mappings are rebuilt by scanning the data file, so the file is truncated to `virtual_size` when opened.

## Limitations

- Snapshots are not supported with a warpfile upper layer (`upper.target`).
- With a hybrid or sparse upper, writes do not take the snapshot's write lock, so the caller must pause writes while a snapshot runs.
- Snapshots on one device are serialized; a running snapshot holds the device's write path.
