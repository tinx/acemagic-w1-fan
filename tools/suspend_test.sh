#!/bin/bash
# Suspend/resume test for the installed acemagic_w1_fan module: log the fan,
# suspend (s2idle) with an RTC wake alarm, then log how the fan comes back.
# Run as root; detached so an SSH session freezing during suspend doesn't
# kill it:  sudo systemd-run --unit=fan-suspend-test /path/to/tools/suspend_test.sh
# Output: suspend_test.log
set -eu
cd "$(dirname "$0")"
LOG=suspend_test.log
OWNER=$(stat -c %U .)
SLEEP_S=30

exec > >(tee "$LOG") 2>&1
trap 'chown "$OWNER" "$LOG" 2>/dev/null || true' EXIT

HW=$(grep -l '^acemagic_w1_fan$' /sys/class/hwmon/hwmon*/name | xargs dirname)
[ -n "$HW" ] || { echo "acemagic_w1_fan not loaded"; exit 1; }

show() {  # label samples interval
    for _ in $(seq "$2"); do
        printf '%s %-7s temp=%3dC enable=%s pwm1=%3s fan=%4s rpm\n' "$(date +%T.%N | cut -c1-12)" "$1" \
            $(( $(cat "$HW/temp1_input") / 1000 )) "$(cat "$HW/pwm1_enable")" \
            "$(cat "$HW/pwm1")" "$(cat "$HW/fan1_input")"
        sleep "$3"
    done
}

start=$(date '+%Y-%m-%d %H:%M:%S')
show before 5 1
echo "$(date +%T) suspending (s2idle) for ${SLEEP_S}s"
rtcwake -m freeze -s "$SLEEP_S"
echo "$(date +%T) resumed"
show after 20 0.5

echo "--- kernel log since test start:"
journalctl -k --since "$start" --no-pager | grep -E 'PM: suspend|acemagic|ACPI: EC|Freezing|Restarting' || true
