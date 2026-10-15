#!/usr/bin/env python3
"""Guarded fan test: lower IT5570E PWM2 duty (DCR2 @ 0x1804) in steps via I2EC,
watch the fan RPM, then restore the original value.

The ONLY EC address this script can write is DCR2 (0x1804). Original value is
restored after the test and on any exit (Ctrl+C included).

Run:  sudo python3 -I i2ec_fantest.py      Output: i2ec_fantest.log
"""
import os, sys, time

CFG_IDX, CFG_DAT = 0x4E, 0x4F
D2ADR, D2DAT = 0x2E, 0x2F
I2EC_ADDR_L, I2EC_ADDR_H, I2EC_DATA = 0x10, 0x11, 0x12
DCR2, CTR1 = 0x1804, 0x1841
F2TLRR, F2TMRR = 0x1820, 0x1821
WRITABLE = {DCR2}
FS = 9.2e6 / 128                 # tach sampling rate (FreqEC / 128)
STEPS = [0xA0, 0x78, 0x50]       # ~80 %, 60 %, 40 % of CTR1+1 = 201
HOLD_S = 10
MAX_TEMP = 70
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "i2ec_fantest.log")

fd = os.open("/dev/port", os.O_RDWR)
logf = open(LOG, "a")


def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line)
    logf.write(line + "\n")
    logf.flush()


def outb(port, val):
    os.pwrite(fd, bytes([val]), port)


def inb(port):
    return os.pread(fd, 1, port)[0]


def cfg_read(idx):
    outb(CFG_IDX, idx)
    return inb(CFG_DAT)


def cfg_write(idx, val):
    assert idx in (D2ADR, D2DAT)
    outb(CFG_IDX, idx)
    outb(CFG_DAT, val)


def set_addr(addr):
    cfg_write(D2ADR, I2EC_ADDR_H)
    cfg_write(D2DAT, addr >> 8)
    cfg_write(D2ADR, I2EC_ADDR_L)
    cfg_write(D2DAT, addr & 0xFF)
    cfg_write(D2ADR, I2EC_DATA)
    outb(CFG_IDX, D2DAT)


def ec_read(addr):
    set_addr(addr)
    return inb(CFG_DAT)


def ec_write(addr, val):
    assert addr in WRITABLE, f"refusing to write EC 0x{addr:04x}"
    set_addr(addr)
    outb(CFG_DAT, val)


def rpm():
    # LSB/MSB read via I2EC isn't atomic; take the median of 3 reads
    vals = []
    for _ in range(3):
        t = ec_read(F2TLRR) | ec_read(F2TMRR) << 8
        vals.append(round(60 * FS / (t * 2)) if t else 0)
    return sorted(vals)[1]


def cpu_temp():
    for h in os.listdir("/sys/class/hwmon"):
        p = f"/sys/class/hwmon/{h}"
        if open(f"{p}/name").read().strip() == "k10temp":
            return int(open(f"{p}/temp1_input").read()) // 1000
    raise RuntimeError("k10temp not found")


def main():
    saved_idx = inb(CFG_IDX)
    if (cfg_read(0x20), cfg_read(0x21)) != (0x55, 0x70):
        sys.exit("Unexpected chip ID, aborting.")
    saved_d2adr = cfg_read(D2ADR)
    if (ec_read(0x2000), ec_read(0x2001)) != (0x55, 0x70) or ec_read(0x200D) & 3 != 3:
        sys.exit("I2EC not read-write, aborting.")
    orig, ctr = ec_read(DCR2), ec_read(CTR1)
    log(f"=== fan test: DCR2=0x{orig:02x} CTR1=0x{ctr:02x} rpm={rpm()} temp={cpu_temp()}C")
    try:
        for duty in STEPS:
            if cpu_temp() > MAX_TEMP:
                log(f"CPU above {MAX_TEMP}C, stopping")
                break
            ec_write(DCR2, duty)
            log(f"--- DCR2 = 0x{duty:02x} ({100 * duty / (ctr + 1):.0f}%)")
            for s in range(HOLD_S):
                time.sleep(1)
                cur = ec_read(DCR2)
                log(f"  t+{s + 1:2d}s rpm={rpm():5d} temp={cpu_temp()}C DCR2=0x{cur:02x}"
                    + ("" if cur == duty else "  <- changed by firmware"))
                if cpu_temp() > MAX_TEMP:
                    break
    finally:
        ec_write(DCR2, orig)
        back = ec_read(DCR2)
        log(f"restored DCR2=0x{back:02x} ({'OK' if back == orig else 'MISMATCH!'})")
        time.sleep(5)
        log(f"rpm 5 s after restore: {rpm()}")
        cfg_write(D2ADR, saved_d2adr)
        outb(CFG_IDX, saved_idx)
        logf.close()
        os.chown(LOG, int(os.environ.get("SUDO_UID", 0)), int(os.environ.get("SUDO_GID", 0)))


if __name__ == "__main__":
    main()
