# Corsair Commander Duo — Linux hwmon Driver & GPU Fan Control

## Overview

This project provides a Linux kernel HID driver for the **Corsair Commander Duo** (USB VID=`0x1b1c`, PID=`0x0c56`) and a GPU-temperature-driven fan control service using `nvidia-smi` as the temperature source.

The system exposes two temperature sensors and two fan RPM inputs via the Linux `hwmon` subsystem (`sensors`), and provides `pwm1`/`pwm2` sysfs attributes for fan speed control.

---

## Hardware

| Property | Value |
|---|---|
| Device | Corsair Commander Duo |
| USB VID | `0x1b1c` |
| USB PID | `0x0c56` |
| HID interfaces | 2 — driver binds to interface 0 only |
| Fans | 2 channels (only one fan connected in current setup) |
| Temp sensors | 2 channels |

---

## Protocol (BRAGI variant, 64-byte HID reports)

```
OUT: [0x00][0x08][handle][opcode][prop][data...]   (report ID 0x00 + 64-byte payload)
IN:  [0x00][handle][error][dtype][data...]
```

### Handle init (opcode 0x03)
```
[0x08, h, 0x03, 0x00, 0x02]
```
Opens a handle. Some handles time out — that is expected and handled.

### iCUE polling cycle (5 commands)
```
[08 0d 01 17] → [08 09 01 00] → [08 08 01 00] → [08 05 01 01] → [08 0d 01 21]
```
The `08 08 01 00` response alternates between:
- **dtype `0x06`** — fan RPM data
- **dtype `0x10`** — temperature data

### Fan RPM data (dtype `0x06`)
```
resp[5]         = fan count
resp[6 + n*2]   = rpm_lo  (LE16)
resp[7 + n*2]   = rpm_hi
```

### Temperature data (dtype `0x10`)
```
resp[5]         = sensor count
resp[6 + n*3]   = status (0x00 = OK)
resp[7 + n*3]   = temp_lo  (LE16, deci-°C)
resp[8 + n*3]   = temp_hi
```
Convert: `raw * 100` → millidegrees C for hwmon.

### Fan data location — critical

Fan RPM data appears in the **init buffer**, not just in polling cycles:
1. The response to the `h=0x01` init command itself contains dtype `0x06`
2. An **immediate `h=0x08` read** right after init also returns dtype `0x06`

These only appear on a **fresh HID session** (after USB replug or driver reload). The driver captures both explicitly in `csduo_reopen_handles()`.

### PWM write cycle (confirmed from pcap)
```
[08 0d 01 18] → [08 09 01 00] → [08 06 01 07 00 00 00 07 00 01 00 00 <duty%>] → [08 05 01 01]
```
- `duty%` range: `0x00`–`0x64` (0–100%)
- Linux hwmon uses 0–255; driver scales: `duty_pct = (val * 100) / 255`
- **The device does not latch speed** — it reverts to default if commands stop. The driver uses a `delayed_work` to resend every 1 second.

---

## Driver Architecture (`corsair-cduo.c`)

### Key design decisions

| Decision | Rationale |
|---|---|
| Lazy init | Device is not polled until first `sensors` read |
| `extra_groups` for PWM | `HWMON_CHANNEL_INFO(pwm, ...)` returns `-EINVAL` on kernel 6.8; custom sysfs attributes via `extra_groups` work correctly |
| `pwm_active[]` flag | Tracks whether user has explicitly written a speed; avoids using a sentinel value like `255` that would conflict with valid 100% commands |
| `delayed_work` | Resends active PWM commands every 1 second so the device maintains speed regardless of sensor polling frequency |
| 4-cycle poll | Device alternates dtype on each cycle; up to 4 cycles ensures both fan and temp data are captured |

### struct csduo_data
```c
struct csduo_data {
    struct hid_device *hdev;
    struct device *hwmon_dev;
    struct mutex lock;
    struct completion wait_input;
    struct delayed_work pwm_work;   /* periodic PWM refresh */
    u8 *cmd_buffer;
    u8 resp[PKT_LEN];
    bool initialized;
    unsigned long temp_updated;
    long temp_cache[NUM_TEMPS];     /* millidegrees C */
    bool temp_valid[NUM_TEMPS];
    long fan_cache[NUM_FANS];       /* RPM */
    bool fan_valid[NUM_FANS];
    u8 pwm_cache[NUM_FANS];         /* last-written value, 0–255 */
    bool pwm_active[NUM_FANS];      /* true once user has written a value */
};
```

### sysfs files exposed (hwmon device)

| File | Access | Description |
|---|---|---|
| `temp1_input` | r | Temperature sensor 1 (millidegrees C) |
| `temp2_input` | r | Temperature sensor 2 |
| `fan1_input` | r | Fan 1 RPM |
| `fan2_input` | r | Fan 2 RPM |
| `pwm1` | rw | Fan 1 PWM (0–255) |
| `pwm2` | rw | Fan 2 PWM (0–255) |
| `pwm1_enable` | rw | Always returns 1 (manual mode); writes accepted but ignored |
| `pwm2_enable` | rw | Same |

### Known issues / gotchas

- **`HWMON_CHANNEL_INFO(pwm, ...)` causes `-EINVAL` on kernel 6.8.**
  Root cause: kernel validates write callback at registration time.
  Workaround: use `extra_groups` with `DEVICE_ATTR_RW` — works cleanly.

- **Device state degrades after multiple `rmmod`/`insmod` without USB replug.**
  Fan data stops appearing. Fix: unplug/replug Commander Duo. The driver correctly captures fan data on fresh sessions via the init buffer reads in `csduo_reopen_handles`.

- **Device does not latch fan speed.**
  iCUE sends speed commands continuously (~2 Hz). The driver replicates this with a `delayed_work` at 1 Hz.

- **Both channels use the same PCap-confirmed command** (`07` bitmask in the fan command may target all fans). Individual channel control is implemented but untested with two fans connected.

---

## GPU Fan Control Service (`csduo-fancontrol.py`)

Uses `nvidia-smi` as the temperature source (GPU die temp, not exhaust sensor) and adjusts both fan channels on the same curve.

### Fan curve

| GPU Temp | PWM | Approx % |
|---|---|---|
| ≤ 30°C | 77 | 30% |
| 40°C | 115 | 45% |
| 50°C | 153 | 60% |
| 55°C | 191 | 75% |
| ≥ 65°C | 255 | 100% |

Values between breakpoints are linearly interpolated. Edit the `CURVE` list in `/usr/local/bin/csduo-fancontrol.py` to adjust.

### Why nvidia-smi instead of fancontrol?

- `fancontrol` reads only hwmon sysfs — cannot read `nvidia-smi` directly
- GPU exhaust sensors (Commander Duo temp1/temp2) have thermal lag vs. die temp
- `nvidia-smi` gives actual GPU die temperature, more responsive for fan curves
- Custom script is simpler and more flexible than `fancontrol` config for this use case

---

## Build & Deploy

See `deploy.sh` in this directory. It handles everything from source.

### Manual steps (for reference)

```sh
# On the target machine:
cd ~/corsair-cduo
make clean && make
sudo insmod corsair-cduo.ko
sudo dmesg | tail -5
sensors
```

If fans show N/A: unplug/replug the Commander Duo USB, then `sudo insmod` again.

### Driver auto-load on boot

```sh
cd ~/corsair-cduo
sudo make install
echo "corsair_cduo" | sudo tee /etc/modules-load.d/corsair-cduo.conf
```

---

## File Reference

| File | Location | Purpose |
|---|---|---|
| `corsair-cduo.c` | `corsair-cduo/` | Kernel HID driver source |
| `Makefile` | `corsair-cduo/` | Kernel module build |
| `csduo-fancontrol.py` | `corsair-cduo/` (source) → `/usr/local/bin/` (installed) | GPU fan control script |
| `csduo-fancontrol.service` | `corsair-cduo/` (source) → `/etc/systemd/system/` (installed) | systemd service unit |
| `deploy.sh` | `corsair-cduo/` | One-shot build + install script |
| `NOTES.md` | `corsair-cduo/` | This document |

### Service management

```sh
sudo systemctl status csduo-fancontrol
sudo systemctl restart csduo-fancontrol
sudo journalctl -u csduo-fancontrol -f    # follow live output
```

---

## Python Test Scripts (development reference)

| Script | Purpose |
|---|---|
| `duo_fan_confirm.py` | Confirmed fan speed control via hidraw (iCUE-continuous mode) |
| `duo_fan_speed_raw.py` | Contains pcap-confirmed PWM command |
| `duo_test.py` | Reveals fan data in init buffer (key protocol discovery) |
| `duo_feature_probe.py` | HID feature report experiments |
| `duo_usbreset_test.py` | USB reset + handle probe after reset |
| `duo_fwreset_test.py` | Firmware reset + handle re-discovery |
