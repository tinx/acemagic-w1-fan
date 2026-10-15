#!/usr/bin/env python3
"""Guarded EC write-probe: find the fan duty register on the Acemagic W1.

For each candidate register: lower its value, watch fan RPM for a few seconds,
then restore the original. All touched bytes are restored on any exit.

Run:  sudo python3 -I probe.py            (writes to the EC!)
      sudo python3 -I probe.py --dry-run  (reads only, prints the plan)
Output: probe.log
"""
import os, subprocess, sys, time

EC = "/sys/kernel/debug/ec/ec0/io"
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probe.log")
RPM_HI, RPM_LO = 0x22, 0x23      # fan RPM, big-endian
MAX_TEMP = 70                     # °C, abort the test step above this
WATCH_S = 8                       # seconds to watch after each write
SETTLE_S = 15                     # max seconds to wait for RPM recovery

# (label, [(register, test_value_fn)]) — multi-byte entries are written high byte first.
PLAN = [
    ("0x63 halved",            [(0x63, lambda v: v // 2)]),
    ("0x62 halved",            [(0x62, lambda v: v // 2)]),
    ("0x68-69 LE16 halved",    [(0x69, None), (0x68, None)]),   # filled in below
    ("0x66-67 LE16 halved",    [(0x67, None), (0x66, None)]),
    ("0x60 flag 1->0",         [(0x60, lambda v: 0 if v else 1)]),
]

dry = "--dry-run" in sys.argv
touched = {}  # reg -> original value
logf = open(LOG, "a")


def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line)
    logf.write(line + "\n")
    logf.flush()


def dump(fd):
    return os.pread(fd, 256, 0)


def rpm(d):
    return d[RPM_HI] << 8 | d[RPM_LO]


def cpu_temp():
    for h in os.listdir("/sys/class/hwmon"):
        p = f"/sys/class/hwmon/{h}"
        if open(f"{p}/name").read().strip() == "k10temp":
            return int(open(f"{p}/temp1_input").read()) // 1000
    raise RuntimeError("k10temp not found")


def write(fd, reg, val):
    if dry:
        log(f"  [dry] would write 0x{reg:02X} = 0x{val:02X}")
        return
    os.pwrite(fd, bytes([val]), reg)


def restore_all(fd):
    for reg, orig in touched.items():
        write(fd, reg, orig)
        log(f"  restored 0x{reg:02X} = 0x{orig:02X}")
    touched.clear()


def le16_halved(d, lo, hi):
    v = (d[lo] | d[hi] << 8) // 2
    return {hi: v >> 8, lo: v & 0xFF}


def main():
    subprocess.run(["modprobe", "-r", "ec_sys"], check=False)
    subprocess.run(["modprobe", "ec_sys", f"write_support={0 if dry else 1}"], check=True)
    fd = os.open(EC, os.O_RDONLY if dry else os.O_RDWR)
    try:
        base = dump(fd)
        log(f"=== probe start (dry={dry}) rpm={rpm(base)} temp={cpu_temp()}C")
        log("baseline: " + base.hex(" "))
        for label, steps in PLAN:
            d = dump(fd)
            if cpu_temp() > MAX_TEMP:
                log(f"SKIP {label}: CPU above {MAX_TEMP}C")
                continue
            if label.startswith("0x68"):
                targets = le16_halved(d, 0x68, 0x69)
            elif label.startswith("0x66"):
                targets = le16_halved(d, 0x66, 0x67)
            else:
                targets = {reg: fn(d[reg]) for reg, fn in steps}
            before = rpm(d)
            log(f"--- {label}: rpm before={before}")
            for reg, _ in steps:  # keep plan order (high byte first)
                touched[reg] = d[reg]
                write(fd, reg, targets[reg])
                log(f"  0x{reg:02X}: 0x{d[reg]:02X} -> 0x{targets[reg]:02X}")
            for s in range(WATCH_S):
                time.sleep(1)
                cur = dump(fd)
                stuck = all(cur[r] == targets[r] for r in targets)
                log(f"  t+{s+1}s rpm={rpm(cur)} temp={cpu_temp()}C value_stuck={stuck}")
                if cpu_temp() > MAX_TEMP:
                    log("  ABORT step: temperature limit")
                    break
            after = dump(fd)
            changed = [f"0x{i:02X}:{d[i]:02X}->{after[i]:02X}" for i in range(256)
                       if d[i] != after[i] and i not in (RPM_HI, RPM_LO, 0x98, 0x99)]
            log(f"  other bytes changed: {changed or 'none'}")
            restore_all(fd)
            for _ in range(SETTLE_S):  # let fan return to baseline before next test
                time.sleep(1)
                if abs(rpm(dump(fd)) - before) < 100:
                    break
            log(f"  rpm after restore={rpm(dump(fd))}")
        final = dump(fd)
        diff = [f"0x{i:02X}" for i in range(256) if final[i] != base[i]
                and i not in (RPM_HI, RPM_LO, 0x98, 0x99, 0x26, 0x27)]
        log(f"=== done. bytes differing from baseline: {diff or 'none'}")
    finally:
        restore_all(fd)
        os.close(fd)
        subprocess.run(["modprobe", "-r", "ec_sys"], check=False)
        subprocess.run(["modprobe", "ec_sys"], check=False)  # back to read-only
        os.chown(LOG, int(os.environ.get("SUDO_UID", 0)), int(os.environ.get("SUDO_GID", 0)))


if __name__ == "__main__":
    main()
