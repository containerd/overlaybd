#!/usr/bin/env python3
"""ublk pool concurrency smoke test for overlaybd-ublkd.

Each worker acquires its own image, all workers release at the same time while
a lister keeps polling /v1/list, then the script checks that every lease was
released. Exits non-zero on any failed request or leftover device.

Usage: pool_stress.py <socket> <rounds> <image_config> [<image_config> ...]
"""
import json
import socket
import sys
import threading
import time

SOCK = sys.argv[1]
ROUNDS = int(sys.argv[2])
IMGS = sys.argv[3:]

lock = threading.Lock()
failures = []
barrier = threading.Barrier(max(1, len(IMGS)))
stop = False


def fail(msg):
    with lock:
        failures.append(msg)
    barrier.abort()


def http(method, path, body=None, timeout=15):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    data = json.dumps(body).encode() if body is not None else b""
    s.sendall(
        (
            f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n"
            f"Content-Length: {len(data)}\r\nConnection: close\r\n\r\n"
        ).encode()
        + data
    )
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    head, _, payload = buf.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1].strip())
    while len(payload) < clen:
        chunk = s.recv(65536)
        if not chunk:
            break
        payload += chunk
    s.close()
    parts = head.split(b"\r\n")[0].split()
    status = int(parts[1]) if len(parts) >= 2 else 0
    return status, payload[:clen].decode(errors="replace")


def worker(img):
    for _ in range(ROUNDS):
        if stop:
            return
        dev = None
        for _ in range(200):
            try:
                code, body = http("POST", "/v1/acquire",
                                  {"config": img, "mode": "exclusive"})
            except Exception as e:  # noqa: BLE001
                fail(f"acquire {img}: {e}")
                return
            if code == 200:
                try:
                    dev = json.loads(body)["dev_id"]
                except (ValueError, KeyError, TypeError) as e:
                    fail(f"acquire {img}: bad response {body[:120]} ({e})")
                    return
                break
            if code == 409:
                time.sleep(0.005)
                continue
            fail(f"acquire {img} -> {code}: {body[:120]}")
            return
        if dev is None:
            fail(f"acquire {img}: no device id")
            return
        try:
            barrier.wait(timeout=30)
        except Exception:  # noqa: BLE001
            pass
        for _ in range(200):
            try:
                code, body = http("POST", "/v1/release", {"dev_id": dev})
            except Exception as e:  # noqa: BLE001
                fail(f"release {dev}: {e}")
                return
            if code == 200:
                break
            if code == 409:
                time.sleep(0.005)
                continue
            fail(f"release {dev} -> {code}: {body[:120]}")
            return


def lister():
    while not stop:
        try:
            code, body = http("GET", "/v1/list")
        except Exception as e:  # noqa: BLE001
            fail(f"list: {e}")
            return
        if code != 200:
            fail(f"list -> {code}: {body[:120]}")
            return
        time.sleep(0.002)


threads = [threading.Thread(target=worker, args=(img,), daemon=True) for img in IMGS]
for t in threads:
    t.start()
lt = threading.Thread(target=lister, daemon=True)
lt.start()
for t in threads:
    t.join()
stop = True
lt.join(timeout=5)

try:
    code, body = http("GET", "/v1/list")
    if code == 200:
        devices = json.loads(body).get("devices", [])
        if devices:
            fail(f"devices still listed after release: {devices}")
    else:
        fail(f"final list -> {code}: {body[:120]}")
except Exception as e:  # noqa: BLE001
    fail(f"final list: {e}")

try:
    code, body = http("GET", "/v1/pool")
    print("pool:", body[:200])
except Exception as e:  # noqa: BLE001
    fail(f"final pool: {e}")

if failures:
    print(f"FAILED ({len(failures)})")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print(f"OK: {len(IMGS)} workers x {ROUNDS} rounds")
