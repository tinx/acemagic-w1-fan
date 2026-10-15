#!/bin/bash
# Test-load driver/acemagic_w1_fan.ko (built in place with `make -C driver`)
# without installing it: idle -> CPU load -> cooldown in auto mode, then a
# quick check of full-speed and manual modes, then unload and verify the
# firmware duty is restored. Run with sudo bash. Output: module_test.log
set -eu
cd "$(dirname "$0")"
KO=../driver/acemagic_w1_fan.ko
LOG=module_test.log
OWNER="${SUDO_USER:-root}"

exec > >(tee "$LOG") 2>&1

[ -f "$KO" ] || { echo "build first: make -C driver"; exit 1; }
if [ -d /sys/module/acemagic_w1_fan ]; then
    echo "module already loaded, unloading first"; rmmod acemagic_w1_fan
fi

pids=""
cleanup() {
    [ -n "$pids" ] && kill $pids 2>/dev/null || true
    [ -d /sys/module/acemagic_w1_fan ] && rmmod acemagic_w1_fan || true
    chown "$OWNER" "$LOG" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

insmod "$KO"
dmesg | grep acemagic_w1_fan | tail -3
HW=$(grep -l '^acemagic_w1_fan$' /sys/class/hwmon/hwmon*/name | xargs dirname)
echo "hwmon: $HW"
for a in pwm1_auto_point1_temp pwm1_auto_point1_pwm pwm1_auto_point4_temp pwm1_auto_point4_pwm; do
    echo "  $a = $(cat "$HW/$a")"
done

show() {  # phase seconds
    end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        printf '%s %-9s temp=%3dC enable=%s pwm1=%3s fan=%4s rpm\n' "$(date +%T)" "$1" \
            $(( $(cat "$HW/temp1_input") / 1000 )) "$(cat "$HW/pwm1_enable")" \
            "$(cat "$HW/pwm1")" "$(cat "$HW/fan1_input")"
        sleep 2
    done
}

show idle 30
for _ in $(seq "$(nproc)"); do yes > /dev/null & pids="$pids $!"; done
show load 90
kill $pids; pids=""
show cooldown 90

echo 0 > "$HW/pwm1_enable"; show full 10
echo 1 > "$HW/pwm1_enable"; echo 128 > "$HW/pwm1"; show manual 10
echo 20 > "$HW/pwm1"; echo "wrote pwm1=20, expect clamp to min_pwm:"; show manual 6
echo 2 > "$HW/pwm1_enable"; show auto 6

rmmod acemagic_w1_fan
echo "unloaded; firmware duty should be back to 100 %:"
sleep 5
python3 -I ./i2ec_read.py | grep -E 'PWM2|tach'
