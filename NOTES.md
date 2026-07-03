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
| Response-to-command matching | Responses echo the command opcode in byte 1; `csduo_raw_event` drops any report whose opcode doesn't match the in-flight command, so a late reply to a timed-out command can't satisfy the next caller or shift subsequent exchanges off by one |
| Parse before close | The read response is snapshotted before the trailing close command, whose reply overwrites the response buffer (see the buffer-echo quirk below) |
| Self-heal on poll failure | `csduo_ensure_fresh()` re-enters software mode when a poll fails — transport error or parse failure (error byte / wrong dtype). A successful recovery logs at `hid_info` |
| PWM restore after re-init | Re-entering software mode can reset the device's commanded duty, so `csduo_init_device()` replays every userspace-written PWM channel after a successful (re-)init |
| Exact PWM readback | `pwm1`/`pwm2` read back exactly the last value written (the 0–255 → percent conversion is not round-tripped) |

### sysfs attributes (hwmon)

| File | Access | Description |
|---|---|---|
| `temp1_input` | RO | Temperature sensor 1 (millidegrees C) |
| `temp2_input` | RO | Temperature sensor 2 |
| `fan1_input` | RO | Fan 1 RPM |
| `fan2_input` | RO | Fan 2 RPM |
| `pwm1` | RW | Fan 1 duty cycle (0-255) |
| `pwm2` | RW | Fan 2 duty cycle (0-255) |
| `firmware_version` | RO | Device firmware version string (e.g. `0.8.105`) |
| `temp1_label` | RO | "Probe 1" |
| `temp2_label` | RO | "Probe 2" |
| `fan1_label` | RO | "Fan 1" |
| `fan2_label` | RO | "Fan 2" |

### Known quirks

These are behaviors of the device/firmware, not of the driver. The driver
handles all of them — none require user action.

- **The device intermittently drops its software-mode session.**
  Observed after USB power-management events and extended idle. In that state
  the fan endpoint answers reads with error byte `0x03` and the wrong dtype,
  so sensor reads would fail while PWM appears fine. **The driver recovers
  automatically**: on a failed poll it re-enters software mode, re-polls, and
  restores any commanded fan speeds. A successful recovery logs
  `recovered from failed poll (...) by re-entering software mode` to dmesg at
  info level. (This same session drop was the cause of historical "fan data
  disappears after repeated `rmmod`/`insmod`" symptoms — no USB replug is
  needed, re-entering software mode is the actual fix.)

- **Close/open responses echo the previous data buffer.**
  The reply to a close (`[08 05 01 fc]`) or open (`[08 0d fc xx]`) command
  carries the err/dtype/data bytes of the device's *previous* response, with
  only the opcode-echo byte (`resp[1]`) changed. Confirmed on firmware
  0.8.105 via dynamic-debug capture:

  ```
  read  resp [00 08 00 06 00 02 d2 04 ...]   <- fan data
  close resp [00 05 00 06 00 02 d2 04 ...]   <- same payload, opcode swapped
  ```

  **The driver does not rely on this** — read responses are snapshotted
  before the trailing close. The opcode echo in `resp[1]` (which appears in
  every response type) *is* used, for response-to-command matching.

- **`fan_input = 0` can mean "fan present, no tach signal."**
  Some fans don't expose a tachometer wire — they still take PWM
  commands fine, they just never report RPM. The driver reads the device's
  per-channel tach status at probe time (EP `0x1a`, `DTYPE_FAN_STATUS = 0x09`)
  and logs each channel's status to dmesg once. Observed values: `0x03` =
  tach present, `0x01` = no tach signal. Check `dmesg | grep cduo` if a
  `fan_input` of 0 is unexpected.

- **`sensors` displays PWM values on Ubuntu 26.04+ / lm-sensors ≥ 3.6.1.**
  lm-sensors 3.6.1 (December 2023) added PWM sensor support. Ubuntu 24.04 shipped with lm-sensors 3.6.0 (which silently ignored `pwm*` sysfs attributes); Ubuntu 26.04 ships with 3.6.2 (which reads and displays them). The driver's hwmon interface is correct — this is expected behavior. To suppress the output, see the `sensors.conf` ignore directive in README.md.

---

## Tested Kernels

| Kernel | Distribution | Result |
|---|---|---|
| 6.8.0-101-generic | Ubuntu 24.04 (GA) | All features working¹ |
| 6.17.0-29-generic | Ubuntu 24.04 (HWE) | All features working¹ |
| 7.0.0-27-generic | Ubuntu 26.04 | All features working, including the self-heal/hardening revision (live-validated on hardware) |

¹ Tested at an earlier driver revision (before the self-heal and protocol
hardening changes). Those changes introduce no new kernel API usage, so the
supported-kernel floor is unchanged; they just haven't been re-run on these
kernels.

The GA (6.8) and HWE (6.17) kernels exercise both sides of the `unaligned.h`
include split: `<asm/unaligned.h>` on < 6.12 and `<linux/unaligned.h>` on ≥ 6.12.

---

## References

- [FanControl.CorsairLink](https://github.com/EvanMulawski/FanControl.CorsairLink) — CommanderCore protocol implementation (Windows/C#), primary protocol reference
- [MisterZ42/corsair-cpro](https://github.com/MisterZ42/corsair-cpro) — Corsair Commander Pro Linux driver, original inspiration
