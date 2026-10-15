#!/bin/sh
# Read-only data collection for fan-control investigation. Run with sudo.
set -e
cd "$(dirname "$0")"
OWNER="${SUDO_USER:-tabalon}"

apt-get install -y acpica-tools lm-sensors

mkdir -p acpi
cp /sys/firmware/acpi/tables/DSDT /sys/firmware/acpi/tables/SSDT* acpi/

# EC register dump (read-only; ec_sys defaults to write_support=0)
modprobe ec_sys
xxd /sys/kernel/debug/ec/ec0/io > ec_idle.txt

chown -R "$OWNER" acpi ec_idle.txt
echo "Done."
