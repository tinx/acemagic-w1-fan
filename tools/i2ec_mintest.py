#!/usr/bin/env python3
"""Guarded minimum-duty test for the fan on IT5570E PWM2 (DCR2 @ 0x1804).

Phase 1: step duty down from 35 % until the fan stalls (or 0 % is reached).
Phase 2: if it stalled, step duty up from the stall point until it restarts.
The original duty is restored at the end and on any exit (Ctrl+C included).
The ONLY EC address this script can write is DCR2.

Run:  sudo python3 -I i2ec_mintest.py      Output: i2ec_mintest.log
"""
import os, sys, time

CFG_IDX, CFG_DAT = 0x4E, 0x4F
D2ADR, D2DAT = 0x2E, 0x2F
I2EC_ADDR_L, I2EC_ADDR_H, I2EC_DATA = 0x10, 0x11, 0x12
DCR2, CTR1 = 0x1804, 0x1841
F2TLRR, F2TMRR = 0x1820, 0x1821
WRITABLE = {DCR2}
FS = 9.2e6 / 128
DOWN = [0x46, 0x3C, 0x32, 0x28, 0x1E, 0x14, 0x0A, 0x00]   # 35 % ... 0 %
UP_STEP, UP_MAX = 0x0A, 0x50                               # restart search, at most 40 %
HOLD_DOWN_S, HOLD_UP_S = 8, 6
STALL_RPM = 100
MAX_TEMP = 60
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "i2ec_mintest.log")

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


class TooHot(Exception):
    pass


def hold(duty, seconds, ctr):
    """Set duty, sample for `seconds`, return list of RPM samples."""
    ec_write(DCR2, duty)
    log(f"--- DCR2 = 0x{duty:02x} ({100 * duty / (ctr + 1):.0f}%)")
    samples = []
    for s in range(seconds):
        time.sleep(1)
        r, t, cur = rpm(), cpu_temp(), ec_read(DCR2)
        samples.append(r)
        log(f"  t+{s + 1}s rpm={r:5d} temp={t}C"
            + ("" if cur == duty else f"  DCR2=0x{cur:02x} <- changed by firmware"))
        if t > MAX_TEMP:
            raise TooHot()
    return samples


def main():
    saved_idx = inb(CFG_IDX)
    if (cfg_read(0x20), cfg_read(0x21)) != (0x55, 0x70):
        sys.exit("Unexpected chip ID, aborting.")
    saved_d2adr = cfg_read(D2ADR)
    if (ec_read(0x2000), ec_read(0x2001)) != (0x55, 0x70) or ec_read(0x200D) & 3 != 3:
        sys.exit("I2EC not read-write, aborting.")
    orig, ctr = ec_read(DCR2), ec_read(CTR1)
    log(f"=== min-duty test: DCR2=0x{orig:02x} CTR1=0x{ctr:02x} rpm={rpm()} temp={cpu_temp()}C")
    results = []
    try:
        stall = None
        for duty in DOWN:
            s = hold(duty, HOLD_DOWN_S, ctr)
            steady = sorted(s[-3:])[1]
            results.append((duty, steady))
            if all(r < STALL_RPM for r in s[-3:]):
                stall = duty
                log(f"  STALLED at 0x{duty:02x}")
                break
        if stall is None:
            log("Fan never stalled, even at 0 % duty.")
        else:
            duty = stall
            while duty + UP_STEP <= UP_MAX:
                duty += UP_STEP
                s = hold(duty, HOLD_UP_S, ctr)
                if all(r >= STALL_RPM for r in s[-2:]):
                    log(f"  RESTARTED at 0x{duty:02x} ({100 * duty / (ctr + 1):.0f}%), rpm={s[-1]}")
                    break
            else:
                log(f"  no restart up to 0x{UP_MAX:02x}")
    except TooHot:
        log(f"CPU above {MAX_TEMP}C, aborting")
    finally:
        ec_write(DCR2, orig)
        back = ec_read(DCR2)
        log(f"restored DCR2=0x{back:02x} ({'OK' if back == orig else 'MISMATCH!'})")
        time.sleep(5)
        log(f"rpm 5 s after restore: {rpm()}")
        log("summary (duty -> steady rpm): "
            + ", ".join(f"0x{d:02x}->{r}" for d, r in results))
        cfg_write(D2ADR, saved_d2adr)
        outb(CFG_IDX, saved_idx)
        logf.close()
        os.chown(LOG, int(os.environ.get("SUDO_UID", 0)), int(os.environ.get("SUDO_GID", 0)))


if __name__ == "__main__":
    main()
