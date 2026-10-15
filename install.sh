#!/bin/sh
# Idempotently install the acemagic_w1_fan module via DKMS plus a boot-time
# service that rebuilds it if it's missing for the running kernel.
# Run as root. Last line of output is "RESULT: changed" or "RESULT: ok".
set -eu
cd "$(dirname "$0")"

NAME=acemagic-w1-fan
MOD=acemagic_w1_fan
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' driver/dkms.conf)
SRC=/usr/src/$NAME-$VER
KVER=$(uname -r)
SRC_FILES="acemagic_w1_fan.c Makefile dkms.conf"
changed=0
mod_changed=0   # a new build that should replace the loaded module

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
for tool in dkms make gcc; do
    command -v "$tool" >/dev/null || { echo "missing $tool (apt install build-essential dkms)" >&2; exit 1; }
done
[ -d "/lib/modules/$KVER/build" ] || { echo "missing headers (apt install linux-headers-$KVER)" >&2; exit 1; }

dkms_state() {  # prints e.g. "installed", "built", "added" or nothing
    dkms status -m "$NAME" -v "$VER" -k "$KVER" 2>/dev/null | sed -n 's/.*: \([a-z]*\).*/\1/p' | head -1
}

# 1. Source tree in /usr/src matches the repo
src_same=1
for f in $SRC_FILES; do
    cmp -s "driver/$f" "$SRC/$f" || src_same=0
done
if [ "$src_same" -eq 0 ]; then
    if dkms status -m "$NAME" -v "$VER" | grep -q .; then
        dkms remove -m "$NAME" -v "$VER" --all
    fi
    rm -rf "$SRC"
    mkdir -p "$SRC"
    for f in $SRC_FILES; do cp "driver/$f" "$SRC/"; done
    changed=1 mod_changed=1
fi

# 2. Registered and installed for the running kernel
if ! dkms status -m "$NAME" -v "$VER" | grep -q .; then
    dkms add -m "$NAME" -v "$VER"
    changed=1
fi
if [ "$(dkms_state)" != installed ]; then
    dkms install -m "$NAME" -v "$VER" -k "$KVER"
    changed=1 mod_changed=1
fi

# 3. Drop other versions of this package
for old in $(dkms status -m "$NAME" | sed -n 's|^[^/]*/\([^,:]*\).*|\1|p' | sort -u); do
    [ "$old" = "$VER" ] && continue
    dkms remove -m "$NAME" -v "$old" --all
    rm -rf "/usr/src/$NAME-$old"
    changed=1
done

# 4. Boot-time ensure script and service
unit_changed=0
if ! cmp -s scripts/acemagic-w1-fan-ensure /usr/local/sbin/acemagic-w1-fan-ensure; then
    install -m 0755 scripts/acemagic-w1-fan-ensure /usr/local/sbin/
    changed=1
fi
if ! cmp -s scripts/acemagic-w1-fan.service /etc/systemd/system/acemagic-w1-fan.service; then
    install -m 0644 scripts/acemagic-w1-fan.service /etc/systemd/system/
    systemctl daemon-reload
    unit_changed=1
    changed=1
fi
if ! systemctl is-enabled --quiet acemagic-w1-fan.service; then
    systemctl enable acemagic-w1-fan.service
    changed=1
fi

# 5. Running module is the current build
if [ -d "/sys/module/$MOD" ] && [ "$mod_changed" -eq 1 ]; then
    modprobe -r "$MOD"
fi
if [ ! -d "/sys/module/$MOD" ]; then
    modprobe "$MOD"
    changed=1
fi
if [ "$unit_changed" -eq 1 ] || ! systemctl is-active --quiet acemagic-w1-fan.service; then
    systemctl restart acemagic-w1-fan.service
    changed=1
fi

if [ "$changed" -eq 1 ]; then echo "RESULT: changed"; else echo "RESULT: ok"; fi
