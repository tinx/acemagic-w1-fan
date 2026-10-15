#!/usr/bin/env python3
"""READ-ONLY dump of IT5570E internal registers via I2EC (datasheet IT5570 V0.3.1,
sections 6.3.2.9 D2ADR/D2DAT and 7.23.3.5 / Figure 7-41).

Host port writes are limited to the PNPCFG index port (0x4E) and to the I2EC
address latches (depth-2 offsets 0x10/0x11). The I2EC data register (0x12) is
only ever READ, so no EC memory is modified.

Run:  sudo python3 -I i2ec_read.py      Output: i2ec_dump.txt
"""
import os, sys

CFG_IDX, CFG_DAT = 0x4E, 0x4F          # PNPCFG ports (BADRSEL=01b, found by sensors-detect)
D2ADR, D2DAT = 0x2E, 0x2F              # PNPCFG indexes of the depth-2 address/data pair
I2EC_ADDR_L, I2EC_ADDR_H, I2EC_DATA = 0x10, 0x11, 0x12
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "i2ec_dump.txt")

fd = os.open("/dev/port", os.O_RDWR)


def outb(port, val):
    os.pwrite(fd, bytes([val]), port)


def inb(port):
    return os.pread(fd, 1, port)[0]


def cfg_read(idx):
    outb(CFG_IDX, idx)
    return inb(CFG_DAT)


def cfg_write(idx, val):
    assert idx == D2ADR or idx == D2DAT, "only the depth-2 pair may be written"
    outb(CFG_IDX, idx)
    outb(CFG_DAT, val)


def d2_write(off, val):
    assert off in (I2EC_ADDR_L, I2EC_ADDR_H), "I2EC data register is never written"
    cfg_write(D2ADR, off)
    cfg_write(D2DAT, val)


def ec_read(addr):
    d2_write(I2EC_ADDR_H, addr >> 8)
    d2_write(I2EC_ADDR_L, addr & 0xFF)
    cfg_write(D2ADR, I2EC_DATA)
    outb(CFG_IDX, D2DAT)
    return inb(CFG_DAT)


def main():
    lines = []

    def out(s=""):
        print(s)
        lines.append(s)

    saved_idx = inb(CFG_IDX)            # address port reads back last index written
    chip = (cfg_read(0x20), cfg_read(0x21))
    if chip != (0x55, 0x70):
        print(f"Unexpected chip ID {chip[0]:02x}{chip[1]:02x} at 0x4E, aborting.")
        outb(CFG_IDX, saved_idx)
        sys.exit(1)
    saved_d2adr = cfg_read(D2ADR)
    try:
        out(f"PNPCFG chip ID 0x5570, version 0x{cfg_read(0x22):02x}")
        ids = [ec_read(0x2000 + i) for i in range(3)]
        out("I2EC ECHIPID1/2/VER (0x2000-02): " + " ".join(f"{b:02x}" for b in ids))
        if ids[:2] != [0x55, 0x70]:
            out("I2EC does not return the chip ID -> I2EC is disabled (I2ECCTRL=00b). Stopping.")
            return
        badrsel, spctrl1 = ec_read(0x200A), ec_read(0x200D)
        mode = {0: "disabled", 1: "reserved", 2: "READ-ONLY", 3: "READ-WRITE"}[spctrl1 & 3]
        out(f"BADRSEL=0x{badrsel:02x}  SPCTRL1=0x{spctrl1:02x}  -> I2EC is {mode}")
        out()
        out("PWM block 0x1800-0x185F:")
        pwm = [ec_read(0x1800 + i) for i in range(0x60)]
        for row in range(0, 0x60, 16):
            out(f"  {0x1800 + row:04x}: " + " ".join(f"{b:02x}" for b in pwm[row:row + 16]))
        out()
        ctr = [pwm[0x01], pwm[0x41], pwm[0x42], pwm[0x43]]
        pcss = pwm[0x0C] | pwm[0x0D] << 8   # 2 bits per channel: cycle-time group
        out(f"ZTIER=0x{pwm[0x23]:02x} (bit1 PCCE: PWM clock {'on' if pwm[0x23] & 2 else 'OFF'})"
            f"  PWMPOL=0x{pwm[0x0A]:02x}  PCSGR=0x{pwm[0x0F]:02x}")
        out(f"CTR0-3 = {', '.join(f'0x{c:02x}' for c in ctr)}")
        for ch in range(8):
            dcr, grp = pwm[0x02 + ch], (pcss >> (2 * ch)) & 3
            pct = 100 * dcr / (ctr[grp] + 1)
            inv = " (inverted)" if pwm[0x0A] >> ch & 1 else ""
            out(f"  PWM{ch}: DCR=0x{dcr:02x} CTR{grp}=0x{ctr[grp]:02x} -> {pct:5.1f}% duty{inv}")
        tach = pwm[0x1E] | pwm[0x1F] << 8
        out(f"F1 tach raw=0x{tach:04x} (I2EC tach reads may be inconsistent per datasheet)")
    finally:
        cfg_write(D2ADR, saved_d2adr)
        outb(CFG_IDX, saved_idx)
        with open(OUT, "w") as f:
            f.write("\n".join(lines) + "\n")
        os.chown(OUT, int(os.environ.get("SUDO_UID", 0)), int(os.environ.get("SUDO_GID", 0)))


if __name__ == "__main__":
    main()
