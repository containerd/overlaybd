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
cpu_flags=$(awk -F: '/^flags[[:space:]]*:/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}' /proc/cpuinfo)
{
    echo "${label}: CPU flags: ${cpu_flags}"
    if grep -qw avx512f <<<"$cpu_flags"; then
        echo "${label}: AVX-512F is exposed to the runner"
    else
        echo "${label}: AVX-512F is not exposed to the runner"
    fi
} | tee "${result_dir}/${label}-cpu-features.txt"
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
    # That file only reaches the artifact, which needs repository
    # authentication to download. Repeat the daemon's own tail as an annotation
    # so a failure is diagnosable without it - this is what separates a
    # registry or network error from a bug in the backend.
    local state escaped_tail
    state=$(sudo systemctl is-active overlaybd-tcmu 2>/dev/null || true)
    escaped_tail=$(sudo tail -n 25 /var/log/overlaybd.log 2>/dev/null |
        sed -e 's/%/%25/g' -e 's/\r/%0D/g' -e 's/$/%0A/' | tr -d '\n')
    echo "::error title=${label} daemon diagnostics::overlaybd-tcmu=${state}; /var/log/overlaybd.log tail: ${escaped_tail}"
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

echo "${label}: LSMT linearized B+tree selection:"
sudo grep -iE 'linearized b\+tree|linearized bptree' /var/log/overlaybd.log \
    | tee "${result_dir}/${label}-lsmt-index.log" || true
if sudo grep -qi 'using accelerated search for linearized b+tree' /var/log/overlaybd.log; then
    echo "::notice title=${label} LSMT index::AVX-512 accelerated linearized B+tree search enabled"
else
    echo "::notice title=${label} LSMT index::AVX-512 accelerated linearized B+tree search not observed"
fi

# Keep the filesystem mounted as it is in production and select the regular
# file with the most allocated blocks. Use its largest allocated physical
# extent as the raw-device benchmark range: this avoids both the virtual disk's
# unmapped zero ranges and filesystem-level contention between fio jobs.
sudo mount -o ro "$dev" "$mount_dir"
benchmark_file=$(sudo find "$mount_dir" -xdev -type f -size +1M -printf '%b\t%p\n' |
    sort -nr | sed -n '1s/^[^\t]*\t//p')
test -n "$benchmark_file"
filesystem_block_size=$(sudo stat -f -c %S "$mount_dir")
extent_output=$(sudo filefrag -e -b"$filesystem_block_size" "$benchmark_file")
printf '%s\n' "$extent_output" \
    | tee "${result_dir}/${label}-benchmark-file-extents.txt"
read -r extent_start extent_blocks < <(
    printf '%s\n' "$extent_output" | awk '
        /^[[:space:]]*[0-9]+:/ && $0 !~ /unwritten/ {
            start = $4
            end = $5
            sub(/\.\.$/, "", start)
            sub(/:$/, "", end)
            blocks = end - start + 1
            if (blocks > largest) {
                selected = start
                largest = blocks
            }
        }
        END { if (largest > 0) print selected, largest }
    '
)
test -n "${extent_start:-}"
test "${extent_blocks:-0}" -gt 0
benchmark_offset=$((extent_start * filesystem_block_size))
benchmark_size=$((extent_blocks * filesystem_block_size))
test "$benchmark_size" -ge 1048576
echo "${label}: fio data extent: ${benchmark_file}, offset=${benchmark_offset}, size=${benchmark_size}"
printf '%s\n' "$benchmark_file" > "${result_dir}/${label}-benchmark-file.txt"

cache_blocks_before=$(sudo find /opt/overlaybd/registry_cache -type f -printf '%b\n' |
    awk '{blocks += $1} END {print blocks + 0}')
echo "${label}: prewarming ${benchmark_size} data bytes into OverlayBD file cache"
# Use a single outstanding request so this phase measures neither WorkPool
# concurrency nor concurrent cache population; it only establishes identical,
# complete local-cache state for both implementations.
sudo timeout --signal=TERM --kill-after=15s 15m fio \
    --name="${label}-prewarm" \
    --filename="$dev" \
    --offset="$benchmark_offset" \
    --readonly \
    --direct=1 \
    --ioengine=libaio \
    --rw=read \
    --bs=1m \
    --iodepth=1 \
    --numjobs=1 \
    --size="$benchmark_size" \
    --group_reporting=1 \
    --eta=never
cache_blocks_after=$(sudo find /opt/overlaybd/registry_cache -type f -printf '%b\n' |
    awk '{blocks += $1} END {print blocks + 0}')
if [[ "$cache_blocks_after" -le "$cache_blocks_before" ]]; then
    echo "::error title=TCMU cache prewarm failed::${label}: file cache did not grow"
    exit 1
fi

# From this point until cleanup, root processes (including overlaybd-tcmu) have
# no outbound network. fio itself also runs as root, but only accesses the
# selected data extent. A second full read therefore proves every block needed
# by the benchmark can be served from OverlayBD's local file cache.
sudo iptables -I OUTPUT 1 ! -o lo -m owner --uid-owner 0 -j REJECT
ipv4_blocked=1
if command -v ip6tables >/dev/null &&
    sudo ip6tables -I OUTPUT 1 ! -o lo -m owner --uid-owner 0 -j REJECT; then
    ipv6_blocked=1
fi
echo "${label}: verifying the data extent with OverlayBD network access blocked"
sudo timeout --signal=TERM --kill-after=15s 15m fio \
    --name="${label}-offline-cache-verification" \
    --filename="$dev" \
    --offset="$benchmark_offset" \
    --readonly \
    --direct=1 \
    --ioengine=libaio \
    --rw=read \
    --bs=1m \
    --iodepth=1 \
    --numjobs=1 \
    --size="$benchmark_size" \
    --group_reporting=1 \
    --eta=never
echo verified > "${result_dir}/${label}-prewarm-status.txt"

fio_worker_limit=$(nproc)
echo "${label}: fio worker limit: ${fio_worker_limit}"

run_profile() {
    local profile=$1
    local jobs=$2
    local depth=$3
    local run
    test "$jobs" -le "$fio_worker_limit"
    for run in 1 2 3; do
        local output="${result_dir}/${label}-${profile}-${run}.json"
        if ! sudo timeout --signal=TERM --kill-after=10s 40s fio \
            --name="${label}-${profile}" \
            --filename="$dev" \
            --offset="$benchmark_offset" \
            --size="$benchmark_size" \
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
run_profile qd4-j2 2 4
run_profile qd8-j4 4 8
run_profile qd16-j4 4 16
run_profile qd32-j4 4 32
run_profile qd64-j4 4 64
sudo chown -R "$(id -u):$(id -g)" "$result_dir"
sudo systemctl is-active --quiet overlaybd-tcmu ||
    echo "::warning title=OverlayBD service stopped::${label}: service exited during benchmark"
