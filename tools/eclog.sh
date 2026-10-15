#!/bin/sh
# Read-only EC logger: samples all 256 EC bytes + CPU temp every 2 s through
# idle -> full CPU load -> cooldown phases. Run with sudo. Output: eclog.csv
set -e
cd "$(dirname "$0")"
OWNER="${SUDO_USER:-tabalon}"
modprobe ec_sys

EC=/sys/kernel/debug/ec/ec0/io
CPUT=$(grep -l k10temp /sys/class/hwmon/hwmon*/name | head -1 | xargs dirname)/temp1_input
OUT=eclog.csv

sample() {
    printf '%s,%s,%s,%s\n' "$(date +%s)" "$1" "$(cat "$CPUT")" \
        "$(od -An -v -tx1 "$EC" | tr -s ' \n' ' ' | sed 's/^ //; s/ $//')" >> "$OUT"
}

run_phase() {  # name seconds
    end=$(( $(date +%s) + $2 ))
    echo "phase: $1 ($2 s)"
    while [ "$(date +%s)" -lt "$end" ]; do sample "$1"; sleep 2; done
}

echo "time,phase,cpu_millideg,ec_bytes_hex" > "$OUT"
run_phase idle 60

pids=""
for _ in $(seq "$(nproc)"); do yes > /dev/null & pids="$pids $!"; done
trap 'kill $pids 2>/dev/null' EXIT INT TERM
run_phase load 180
kill $pids; trap - EXIT INT TERM

run_phase cooldown 120
chown "$OWNER" "$OUT"
echo "Done: $(wc -l < "$OUT") lines in $OUT"
