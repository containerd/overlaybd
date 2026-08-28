# ADR 0001: Silent Ignore for Unmatched Subtype TCMU Device Requests

## Status

Accepted

## Context

In a multi-instance TCMU deployment, multiple user-space processes each register handlers for different subtypes. The kernel broadcasts `TCMU_CMD_ADDED_DEVICE`, `TCMU_CMD_REMOVED_DEVICE`, and `TCMU_CMD_RECONFIG_DEVICE` via Netlink multicast to all listeners in the "config" group.

Previously, when a process received a device event for a subtype it did not handle, it would:

1. Attempt `device_add()`, fail at `find_handler()`, and return `-ENOENT`
2. Send a Netlink reply (`TCMU_CMD_ADDED_DEVICE_DONE`) with the error code back to the kernel
3. Log an `ERROR`-level message

This caused the kernel to interpret the response as a device creation failure, potentially preventing the correct handler process from claiming the device.

## Decision

When receiving a TCMU device event for a subtype not registered by this process, **silently ignore the message without sending a Netlink reply**. The specific rules are:

1. **ADDED_DEVICE**: `device_add()` returns `-ENODATA` (not `-ENOENT`) when `find_handler()` yields no match. `handle_netlink()` skips `send_netlink_reply()` for this return value.
2. **REMOVED_DEVICE / RECONFIG_DEVICE**: If the device is not in `ctx->devices` (i.e., never claimed by this process), skip the Netlink reply.
3. **Logging**: Subtype mismatch is logged at `DEBUG` level, not `ERROR`. This is expected behavior in multi-instance deployments, not a failure condition.

The return value convention:
- `-ENODATA`: "No handler registered for this subtype" — caller must not reply
- `-ENOENT` / other negatives: "Handler was found but processing failed" — caller should reply with the error

## Consequences

- **Enables multi-instance deployment**: Each process only responds to its own subtypes; the kernel receives exactly one reply from the correct handler.
- **Kernel timeout risk**: If no process handles a given subtype, the kernel will receive no reply and may timeout. This is acceptable — it correctly reflects that no user-space handler is available.
- **Reduced log noise**: Production logs no longer contain spurious ERROR entries for normal cross-subtype traffic.
- **Backward compatible**: Netlink v1 never sent replies anyway; this change only affects v2+ reply behavior for unmatched subtypes.
