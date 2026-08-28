#!/usr/bin/env bash

set -Eeuo pipefail

if [[ $# -ne 5 ]]; then
    echo "usage: $0 <label> <service-config> <image-config> <result-dir> <wwn>" >&2
    exit 2
fi

label=$1
service_config=$2
image_config=$3
result_dir=$4
wwn=$5
mkdir -p "$result_dir"
if [[ "$label" == "baseline" ]]; then
    device_name=vol1
else
    device_name=vol2
fi
core_dir="/sys/kernel/config/target/core/user_1/${device_name}"
fabric_dir="/sys/kernel/config/target/loopback/${wwn}/tpgt_1"
lun_dir="${fabric_dir}/lun/lun_0"
lun_link="${lun_dir}/${device_name}"
mount_dir="${RUNNER_TEMP}/overlaybd-perf-mount-${label}"
ipv4_blocked=0
ipv6_blocked=0

on_error() {
    local rc=$1
    local line=$2
    local command=$3
    trap - ERR
    set +e
    echo "::error title=TCMU benchmark failed::${label}: line ${line}: ${command} (exit ${rc})"
    {
        echo "label: $label"
        echo "line: $line"
        echo "command: $command"
        echo "exit_code: $rc"
        echo
        sudo systemctl status overlaybd-tcmu --no-pager --full
        echo
        sudo tail -n 200 /var/log/overlaybd.log
    } 2>&1 | tee "${result_dir}/${label}-diagnostics.log"
    exit "$rc"
}
trap 'on_error "$?" "$LINENO" "$BASH_COMMAND"' ERR

cleanup() {
    set +e
    if [[ "$ipv6_blocked" -eq 1 ]]; then
        sudo ip6tables -D OUTPUT ! -o lo -m owner --uid-owner 0 -j REJECT || true
        ipv6_blocked=0
    fi
    if [[ "$ipv4_blocked" -eq 1 ]]; then
        sudo iptables -D OUTPUT ! -o lo -m owner --uid-owner 0 -j REJECT || true
        ipv4_blocked=0
    fi
    if mountpoint -q "$mount_dir"; then
        sudo timeout 15 umount "$mount_dir" || true
    fi
    if [[ -L "$lun_link" ]]; then
        sudo unlink "$lun_link" || true
    fi
    if [[ -e "${core_dir}/enable" ]]; then
        echo -n 0 | sudo tee "${core_dir}/enable" >/dev/null || true
    fi
    sudo timeout 15 systemctl stop overlaybd-tcmu >/dev/null 2>&1 || true
    sudo udevadm settle --timeout=10 || true
}
trap cleanup EXIT

cleanup
sudo install -d -m 0755 \
    /etc/overlaybd \
    /opt/overlaybd/registry_cache \
    /var/lib/overlaybd/test \
    "$result_dir" \
    "$mount_dir"
sudo cp "$service_config" /etc/overlaybd/overlaybd.json
sudo find /opt/overlaybd/registry_cache -mindepth 1 -delete
sudo truncate -s 0 /var/log/overlaybd.log
sudo systemctl enable /opt/overlaybd/overlaybd-tcmu.service
sudo systemctl daemon-reload
sudo systemctl start overlaybd-tcmu
sudo systemctl is-active --quiet overlaybd-tcmu

sudo mkdir -p "$core_dir"
echo -n "dev_config=overlaybd/${image_config}" | sudo tee "${core_dir}/control" >/dev/null
echo -n 1 | sudo tee "${core_dir}/enable" >/dev/null
sudo mkdir -p "$lun_dir"
echo -n "$wwn" | sudo tee "${fabric_dir}/nexus" >/dev/null

# Record the runner's existing block devices. The legacy and current TCMU
# implementations do not always expose the same vendor/model text to lsscsi,
# so identify the benchmark disk by the new whole-block device instead.
declare -A blocks_before=()
for block_path in /sys/class/block/*; do
    blocks_before["$(basename "$block_path")"]=1
done
sudo ln -s "$core_dir" "$lun_link"
sudo udevadm settle

dev=""
for _ in $(seq 1 30); do
    for block_path in /sys/class/block/*; do
        block_name=$(basename "$block_path")
        if [[ -z "${blocks_before[$block_name]+present}" &&
              ! -e "${block_path}/partition" &&
              -b "/dev/${block_name}" ]]; then
            dev="/dev/${block_name}"
            break
        fi
    done
    if [[ -n "$dev" && -b "$dev" ]]; then
        break
    fi
    sleep 1
done
lsblk
lsscsi
test -n "$dev"
test -b "$dev"
echo "${label}: benchmarking ${dev}"

# Keep the filesystem mounted as it is in production, but issue fio against the
# raw block device with O_DIRECT so requests cannot be satisfied by its page
# cache before reaching OverlayBD.
sudo mount -o ro "$dev" "$mount_dir"

cache_blocks_before=$(sudo find /opt/overlaybd/registry_cache -type f -printf '%b\n' |
    awk '{blocks += $1} END {print blocks + 0}')
device_bytes=$(sudo blockdev --getsize64 "$dev")
echo "${label}: prewarming all ${device_bytes} bytes into OverlayBD file cache"
# Use a single outstanding request so this phase measures neither WorkPool
# concurrency nor concurrent cache population; it only establishes identical,
# complete local-cache state for both implementations.
sudo timeout --signal=TERM --kill-after=15s 15m fio \
    --name="${label}-prewarm" \
    --filename="$dev" \
    --readonly \
    --direct=1 \
    --ioengine=libaio \
    --rw=read \
    --bs=1m \
    --iodepth=1 \
    --numjobs=1 \
    --size="$device_bytes" \
    --group_reporting=1 \
    --eta=never
cache_blocks_after=$(sudo find /opt/overlaybd/registry_cache -type f -printf '%b\n' |
    awk '{blocks += $1} END {print blocks + 0}')
if [[ "$cache_blocks_after" -le "$cache_blocks_before" ]]; then
    echo "::error title=TCMU cache prewarm failed::${label}: file cache did not grow"
    exit 1
fi

# From this point until cleanup, root processes (including overlaybd-tcmu) have
# no outbound network. fio itself also runs as root, but only accesses the raw
# block device. A second full read therefore proves every block needed by the
# benchmark can be served from OverlayBD's local file cache.
sudo iptables -I OUTPUT 1 ! -o lo -m owner --uid-owner 0 -j REJECT
ipv4_blocked=1
if command -v ip6tables >/dev/null &&
    sudo ip6tables -I OUTPUT 1 ! -o lo -m owner --uid-owner 0 -j REJECT; then
    ipv6_blocked=1
fi
echo "${label}: verifying the full device with OverlayBD network access blocked"
sudo timeout --signal=TERM --kill-after=15s 15m fio \
    --name="${label}-offline-cache-verification" \
    --filename="$dev" \
    --readonly \
    --direct=1 \
    --ioengine=libaio \
    --rw=read \
    --bs=1m \
    --iodepth=1 \
    --numjobs=1 \
    --size="$device_bytes" \
    --group_reporting=1 \
    --eta=never
echo verified > "${result_dir}/${label}-prewarm-status.txt"

run_profile() {
    local profile=$1
    local jobs=$2
    local depth=$3
    local run
    for run in 1 2 3; do
        local output="${result_dir}/${label}-${profile}-${run}.json"
        if ! sudo timeout --signal=TERM --kill-after=10s 40s fio \
            --name="${label}-${profile}" \
            --filename="$dev" \
            --readonly \
            --direct=1 \
            --ioengine=libaio \
            --rw=randread \
            --bs=4k \
            --iodepth="$depth" \
            --numjobs="$jobs" \
            --time_based=1 \
            --runtime=15 \
            --ramp_time=3 \
            --randrepeat=1 \
            --randseed=20260825 \
            --group_reporting=1 \
            --eta=never \
            --output-format=json \
            --output="$output"; then
            sudo unlink "$output" 2>/dev/null || true
            echo timeout > "${result_dir}/${label}-${profile}-${run}-status.txt"
            echo "::error title=TCMU fio timeout::${label}: ${profile} run ${run} did not finish"
            return 1
        fi
    done
}

run_profile qd1-j1 1 1
run_profile qd32-j8 8 32
sudo chown -R "$(id -u):$(id -g)" "$result_dir"
sudo systemctl is-active --quiet overlaybd-tcmu ||
    echo "::warning title=OverlayBD service stopped::${label}: service exited during benchmark"
