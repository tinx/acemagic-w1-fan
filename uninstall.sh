#!/bin/sh
# Remove the acemagic_w1_fan module, its DKMS registration and the boot service.
# Unloading the module restores the firmware's 100 % fan duty. Run with sudo.
set -eu

NAME=acemagic-w1-fan

systemctl disable --now acemagic-w1-fan.service 2>/dev/null || true
modprobe -r acemagic_w1_fan 2>/dev/null || true
for ver in $(dkms status -m "$NAME" | sed -n 's|^[^/]*/\([^,:]*\).*|\1|p' | sort -u); do
    dkms remove -m "$NAME" -v "$ver" --all
    rm -rf "/usr/src/$NAME-$ver"
done
rm -f /etc/systemd/system/acemagic-w1-fan.service /usr/local/sbin/acemagic-w1-fan-ensure
systemctl daemon-reload
echo "Removed $NAME."
