# Fan control investigation — Acemagic W1 ("Warlord Mini")

## Hardware
- AMD Ryzen 7 8745HS, AMI BIOS `PHXPM7B0_V02_01.03` (2024-08-21), DMI all "Default string"
- No Super I/O hwmon chip; fan is driven by the ACPI EC (`\_SB.PCI0.SBRG.EC0`, ports 0x62/0x66)
- Only button is power; it does not change EC mode.
- No public driver / NBFC config / register map found for this board (searched 2026-10-15).

## ACPI (decompiled in `dsl/`)
- EC fields declared: `PERM` @0xFD, `PMBK` @0xFE (DSDT), `PBCN` @0xCC (SSDT13). Nothing fan-related.
- `_Q20` reads PERM -> sets power limits via ALIB: 1=35/40/35 W, 2=40/54/40 W, 3=45/54/45 W.
- Current PERM = 3.

## EC register map (from `ec_idle.txt`, `eclog.csv`)
| Reg | Meaning | Evidence |
|---|---|---|
| 0x22-0x23 | Fan RPM, 16-bit big-endian | ~2020 RPM idle, 2020-2060 at 85 °C |
| 0x98-0x99 | Copy of fan RPM | identical to 0x22 |
| 0x26, 0x27 | CPU temp (°C) | tracks k10temp exactly |
| 0xFD / 0xFE | PERM / PMBK | ACPI |

Static non-zero bytes (duty-register candidates): 0x00=01, 0x01=03, 0x11=10, 0x17=01,
**0x60=01, 0x62=15, 0x63=20, 0x66-67=b0 1d (7600 LE), 0x68-69=88 13 (5000 LE)**, 0xFD=03, 0xFE=03.

## Key finding
EC runs the fan at a **fixed ~2000 RPM regardless of temperature** (no curve). CPU reaches
85 °C under full load at that speed, so any custom curve must stay >= ~2000 RPM when hot.

## Ruled out (2026-10-15)
- BIOS: no fan or performance settings at all.
- Public BIOS/EC updates for W1: none found online (Acemagic support not yet contacted).
- `probe.py` write-probe (see `probe.log`): halved 0x62, 0x63, 0x66-67, 0x68-69 and cleared 0x60,
  8 s each at idle. All writes stuck, all restored cleanly, **zero RPM effect** (2020-2028 throughout,
  no audible change). No other bytes reacted.
- Conclusion: duty control is probably not in the 256-byte ACPI EC window at all (or those
  values are only read at boot / on events).

## EC chip
- `sensors-detect`: Super I/O at 0x4E/0x4F, chip ID **0x5570 = ITE IT5570E** (8051-based EC,
  proprietary OEM firmware). Nothing at 0x2E.
- IT5570E PWM block (from System76 open EC firmware, `src/ec/ite/include/ec/pwm.h`):
  CTR0 0x1801, **DCR0-DCR7 duty 0x1802-0x1809**, PCFSR 0x180B, F1TLRR/F1TMRR tach 0x181E/0x181F,
  CTR1-3 0x1841-0x1843. These are internal EC registers, not in the 256-byte ACPI window.
- Host access path to them (I2EC through Super I/O config space) is NOT verified from any
  public source; coreboot superiotool's IT5570E table doesn't list those registers.
  Don't write through an unverified protocol (wrong address could hit GPIO/power/flash regs).
- Other LPC channels: PMC2 at 0x68/0x6C (unexplored).

## I2EC findings (datasheet `IT5570_A_V0.3.1_U.pdf`, `i2ec_read.py`, `i2ec_dump.txt`)
- I2EC via PNPCFG 0x4E/0x4F: index 0x2E=D2ADR, 0x2F=D2DAT; depth-2 regs 0x10 ADDR_L,
  0x11 ADDR_H, 0x12 DATA (datasheet 6.3.2.9, Fig. 7-41). Works; SPCTRL1(0x200D)=0xC3 -> **read-write**.
- PWM: CTR0-3 all 0xC8. **PWM2 DCR2(0x1804)=0xC8 = CTR1 -> held high = 100% duty** (likely the fan).
  PWM4 DCR4(0x1806)=0x15 (10%), mirrored at ACPI byte 0x62.
- Fan tach is **Fan 2** (F2TLRR/F2TMRR 0x1820/21): RPM = 60*(9.2MHz/128)/(count*2); 0x042B -> 2021 RPM.
- Fan speed is firmware-controlled ("controlled by software"); no hardware auto mode.

## Fan control CONFIRMED (2026-10-15, `i2ec_fantest.log`)
- Writing DCR2 via I2EC changes fan speed; firmware does NOT overwrite it (held 10 s per step).
- 0xC8 (100%) = 2025 RPM, 0xA0 (80%) = ~1786, 0x78 (60%) = ~1404, 0x50 (40%) = ~953. Restore OK.
- Consequence: a value we set stays until changed; a permanent controller must fail safe
  (restore 0xC8 on exit, on errors, and re-apply after resume).
- Min-duty test (`i2ec_mintest.log`): 35%=830, 30%=690, 25%=540, 20%=380, 15%=195 RPM,
  **10% stalls**; restart from stop at 15% is slow (>6 s). Driver floor 20%, default min 25%.
- Untested: behaviour across suspend/resume.

## Kernel module (`driver/`, `scripts/`, `install.sh`, `uninstall.sh`)
- `acemagic_w1_fan`: hwmon fan1_input, temp1_input (EC 0x26), pwm1, pwm1_enable
  (0 full / 1 manual / 2 auto, default 2), pwm1_auto_point[1-4]_{temp,pwm}
  (default 50/60/70/80 C -> 25/40/70/100 %). Params: min_pwm (>=51), crit_temp (85), force.
- Fail-safe: full speed >= crit_temp or temp read error; original DCR restored on unload,
  shutdown, suspend; DCR re-checked every 1 s poll.
- DKMS package `acemagic-w1-fan` 0.1.0; `acemagic-w1-fan.service` runs
  `/usr/local/sbin/acemagic-w1-fan-ensure` at boot (dkms autoinstall if missing, modprobe).
- Build needs build-essential, gcc-15 (kernel built with GCC 15.2), dkms. Secure Boot off.

## Module test load (2026-10-15, `docs/data/module_test.log`)
- Builds cleanly against 7.0.0-38-generic (also with `W=1`).
- Auto mode: ramps 100% -> 25% in ~25 s at idle (~550 RPM). Under all-core load the CPU
  hits 80 C within ~4 s -> 100%; it plateaus at 85 C exactly as with the stock fixed 100%, so
  full load costs no thermal headroom. Cooldown steps back to 25% within ~25 s. Idle at 25%
  settles around 40 C (vs ~28 C at stock 100%).
- Modes: `pwm1_enable=0` -> 100%; manual `pwm1=128` -> ~1210 RPM; `pwm1=20` clamped to 64.
- Unload restores DCR2=0xC8.

## Open items
- `install.sh` ran twice on 2026-10-15: DKMS build installed for 7.0.0-38, service enabled,
  module loaded once. First run `RESULT: changed`, second run `RESULT: ok`. DKMS generated a
  MOK key pair (`/var/lib/shim-signed/mok/`) on first use and signed the module with it.
- Behaviour across suspend/resume.
- After any BIOS/EC update: rerun `tools/i2ec_read.py` and check that I2EC is still read-write and
  that PWM2/CTR1/tach2 look the same before trusting the module (it refuses to load if the
  basic layout checks fail, but can't detect a re-wired fan channel).

## Files
- `docs/data/` — raw results quoted above (`ec_idle.txt`, `eclog.csv`, `probe.log`,
  `i2ec_dump.txt`, `i2ec_fantest.log`, `i2ec_mintest.log`, `module_test.log`).
- `tools/` — the scripts that produced them; all run with `sudo` and write output next to
  themselves:
  - `collect.sh`: ACPI tables and EC dump (read-only).
  - `eclog.sh`: EC/temperature logger over idle, load and cooldown (read-only).
  - `probe.py`: write-probe of ACPI EC bytes (writes!).
  - `i2ec_read.py`: I2EC PWM dump (read-only).
  - `i2ec_fantest.py` and `i2ec_mintest.py`: DCR2 step tests (write DCR2 only).
  - `module_test.sh`: test-load the in-tree build (`make -C driver`) without installing it
    (`sudo bash`).
- Not in the repo: the IT5570 datasheet (`IT5570_A_V0.3.1_U.pdf`) and the ACPI dumps/decompiles
  (`acpi/`, `dsl/`); both are machine-local.
