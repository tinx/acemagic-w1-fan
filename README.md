# acemagic-w1-fan

Linux fan control for the **Acemagic W1 ("Warlord Mini")**: AMD Ryzen 7 8745HS, ITE IT5570E
embedded controller, BIOS `PHXPM7B0_*`.

The stock EC firmware runs the fan at a fixed 100 % duty (~2000 RPM) regardless of
temperature, and the BIOS has no fan settings. This repo provides an out-of-tree hwmon kernel
module, `acemagic_w1_fan`, that drives the fan PWM through the EC's I2EC interface with a
temperature curve. It's packaged for DKMS so the module is rebuilt automatically for new kernels.

How the registers were found, with raw data: [docs/investigation.md](docs/investigation.md).

> **Board-specific.** The module writes an EC-internal PWM register. It only loads when the BIOS
> version starts with `PHXPM7B0`, the IT5570E answers at port 0x4E, I2EC is read-write and the
> PWM layout matches. Don't use `force=1` on other hardware.

## Layout

| Path | Purpose |
|---|---|
| `driver/` | Module source, `Makefile`, `dkms.conf` |
| `scripts/acemagic-w1-fan-ensure` | Boot-time check: build via DKMS if missing for the running kernel, then `modprobe` |
| `scripts/acemagic-w1-fan.service` | systemd oneshot unit running the ensure script |
| `install.sh` / `uninstall.sh` | Idempotent install / full removal (run as root) |
| `docs/` | Investigation notes and raw measurements |
| `tools/` | Investigation scripts (EC dumps, I2EC read and step tests) |

## Requirements

- Packages: `build-essential dkms linux-headers-generic`, plus the kernel headers for the
  running kernel and the GCC version the kernel was built with (see `cat /proc/version`;
  Ubuntu 7.0.0-38-generic was built with GCC 15, i.e. `gcc-15`).
- Secure Boot disabled, or a DKMS MOK enrolled (otherwise the unsigned module won't load).

## Install

```sh
sudo ./install.sh
```

The script is idempotent: it copies the source to `/usr/src/acemagic-w1-fan-<ver>`, registers
and builds it with DKMS for the running kernel, removes other versions, and installs and enables
the boot service. It reloads the module only when the build changed. The last output line is
`RESULT: changed` or `RESULT: ok`.

Kernel updates are covered twice:
- DKMS's kernel postinst hook rebuilds the module when a kernel package is installed.
- `acemagic-w1-fan.service` runs at boot and calls `dkms autoinstall` if the module is missing
  for the running kernel.

If a build fails, the module just doesn't load and the fan stays at the firmware's 100 %.

## Ansible

```yaml
- name: Fan control build dependencies
  ansible.builtin.apt:
    name:
      - build-essential
      - gcc-15
      - dkms
      - linux-headers-generic
      - "linux-headers-{{ ansible_kernel }}"
    state: present

- name: Fetch acemagic-w1-fan
  ansible.builtin.git:
    repo: "{{ acemagic_fan_repo }}"
    dest: /opt/acemagic-w1-fan
    version: "{{ acemagic_fan_version | default('main') }}"

- name: Fan curve options
  ansible.builtin.copy:
    dest: /etc/modprobe.d/acemagic_w1_fan.conf
    content: |
      options acemagic_w1_fan curve_temp=50,60,70,80 curve_pwm=64,102,179,255
    mode: "0644"
  register: acemagic_fan_opts

- name: Install acemagic_w1_fan module
  ansible.builtin.command: /opt/acemagic-w1-fan/install.sh
  register: acemagic_fan_install
  changed_when: acemagic_fan_install.stdout_lines[-1] == 'RESULT: changed'

- name: Reload module for new options
  ansible.builtin.shell: modprobe -r acemagic_w1_fan && modprobe acemagic_w1_fan
  when: acemagic_fan_opts.changed
```

## Usage

The module registers a hwmon device named `acemagic_w1_fan` (`sensors` shows it):

| Attribute | Meaning |
|---|---|
| `fan1_input` | Fan speed in RPM |
| `temp1_input` | CPU temperature as reported by the EC (m°C) |
| `pwm1` | Current duty, 0-255 (writable in manual mode) |
| `pwm1_enable` | `0` full speed, `1` manual, `2` automatic curve (default) |
| `pwm1_auto_point[1-4]_temp` / `_pwm` | Curve points (m°C / 0-255), runtime-adjustable, must stay non-decreasing |

Module options (`/etc/modprobe.d/acemagic_w1_fan.conf`):

| Option | Default | Meaning |
|---|---|---|
| `curve_temp` | `50,60,70,80` | Initial curve temperatures in °C (exactly 4, non-decreasing) |
| `curve_pwm` | `64,102,179,255` | Initial curve duties 0-255 (exactly 4, non-decreasing) |
| `min_pwm` | `64` (25 %) | Lowest duty ever set; clamped to ≥ 51 (20 %) because the fan stalls at ~10 % |
| `crit_temp` | `85` | Full speed at or above this temperature (60-100 °C) |
| `force` | `0` | Skip the BIOS version check |

The curve is linear between points. The fan speeds up immediately and slows down by at most
~3 % per second, with 3 °C of hysteresis.

**Fail-safe behaviour:**
- Full speed at or above `crit_temp`, or if the EC temperature can't be read.
- The firmware's original duty (100 %) is restored on module unload, shutdown and suspend.
- The duty register is re-checked every second and rewritten if something changed it.

## Uninstall

```sh
sudo ./uninstall.sh
```

This unloads the module (the fan returns to 100 %), removes all DKMS versions, the source in
`/usr/src`, and the service.

## After a BIOS/EC update

Rerun `sudo python3 -I tools/i2ec_read.py` and compare with `docs/data/i2ec_dump.txt` before
trusting the module. The module refuses to load if its layout checks fail, but it can't detect
the fan being moved to a different PWM channel.
