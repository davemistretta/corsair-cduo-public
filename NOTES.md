# Corsair Commander Duo — Protocol & Driver Notes

## Hardware

| Property | Value |
|---|---|
| Device | Corsair Commander Duo |
| USB VID | `0x1b1c` |
| USB PID | `0x0c56` |
| HID interfaces | 2 — driver binds to interface 0 only |
| Fan headers | 2 channels |
| Temp sensors | 2 channels |

---

## Protocol (CommanderCore, 64-byte HID reports)

The device uses the CommanderCore protocol with a single communication handle (`0xfc`). All sensor and control operations use an endpoint-based close/open/read or write/close cycle.

### Command format

```
OUT: [0x00][0x08][handle][opcode][data...]   (report ID 0x00 + 64-byte payload)
IN:  [0x00][handle][error][dtype][data...]
```

### Handle operations

| Command | Bytes | Description |
|---|---|---|
| Enter software mode | `[08 01 03 00 02]` | Take control from hardware |
| Enter hardware mode | `[08 01 03 00 01]` | Return control to hardware (sent on driver unload) |
| Close endpoint | `[08 05 01 fc]` | Close current endpoint on handle 0xfc |
| Open endpoint | `[08 0d fc <endpoint>]` | Open an endpoint for reading or writing |
| Read endpoint | `[08 08 fc]` | Read data from currently open endpoint |
| Write endpoint | `[08 06 fc <len_lo> <len_hi> 00 00 <dtype> <data...>]` | Write data to currently open endpoint |

### Endpoints

| Endpoint | Dtype | Direction | Description |
|---|---|---|---|
| `0x17` | `0x06` | Read | Fan RPM |
| `0x21` | `0x10` | Read | Temperature |
| `0x18` | `0x07` | Write | Fan speed control (per-channel, 0-100%) |

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
resp[7 + n*3]   = temp_lo  (LE16, deci-degrees C)
resp[8 + n*3]   = temp_hi
```

Convert: `raw * 100` gives millidegrees C for hwmon.

### Fan speed write (dtype `0x07`)

Per-channel addressing:

```
[08 06 fc 07 00 00 00 07 00 01 <ch_id> 00 <duty%> 00]
```

- `ch_id`: 0-based channel index
- `duty%`: 0x00-0x64 (0-100%)
- Linux hwmon uses 0-255; driver scales: `duty_pct = (val * 100) / 255`
- The device latches the commanded speed; no continuous resend required.

### Sensor read cycle

Each sensor read follows a close-open-read-close pattern:

```
[08 05 01 fc]           close endpoint
[08 0d fc <endpoint>]   open endpoint (0x17 for fans, 0x21 for temps)
[08 08 fc]              read (retry until expected dtype appears)
[08 05 01 fc]           close endpoint
```

The driver reads fan RPM and temperature in separate cycles, with up to 5 retries per read to handle dtype alternation.

---

## Driver Architecture (`corsair-cduo.c`)

### Design decisions

| Decision | Rationale |
|---|---|
| Lazy init | Device is not polled until first sensor read via sysfs |
| 1-second cache | `csduo_ensure_fresh()` skips polling if data is < 1s old |
| Per-channel PWM | Each fan independently addressable via channel ID in write command |
| Hardware mode on remove | `csduo_remove()` sends EnterHardwareMode so device returns to default behavior |
| `HWMON_PWM_INPUT` | Native hwmon PWM channel registration (no workarounds needed) |
| Labels via `read_string` | `hwmon_ops.read_string` provides "Probe 1", "Fan 1", etc. |

### sysfs attributes (hwmon)

| File | Access | Description |
|---|---|---|
| `temp1_input` | RO | Temperature sensor 1 (millidegrees C) |
| `temp2_input` | RO | Temperature sensor 2 |
| `fan1_input` | RO | Fan 1 RPM |
| `fan2_input` | RO | Fan 2 RPM |
| `pwm1` | RW | Fan 1 duty cycle (0-255) |
| `pwm2` | RW | Fan 2 duty cycle (0-255) |
| `temp1_label` | RO | "Probe 1" |
| `temp2_label` | RO | "Probe 2" |
| `fan1_label` | RO | "Fan 1" |
| `fan2_label` | RO | "Fan 2" |

### Known quirks

- **Device state degrades after multiple `rmmod`/`insmod` without USB replug.**
  Fan data may stop appearing. Fix: unplug/replug the Commander Duo USB connector.

---

## Tested Kernels

| Kernel | Distribution | Result |
|---|---|---|
| 6.8.0-106-generic | Ubuntu 24.04 | All features working |
| 6.14.0-37-generic | Ubuntu 24.04 (HWE) | All features working |

---

## References

- [FanControl.CorsairLink](https://github.com/EvanMulawski/FanControl.CorsairLink) — CommanderCore protocol implementation (Windows/C#), primary protocol reference
- [MisterZ42/corsair-cpro](https://github.com/MisterZ42/corsair-cpro) — Corsair Commander Pro Linux driver, original inspiration
