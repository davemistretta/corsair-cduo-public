# corsair-cduo

Linux kernel HID driver for the **Corsair Commander Duo** (USB VID `0x1b1c`, PID `0x0c56`).

Exposes fan RPM, temperature sensors, and per-channel PWM fan speed control via the standard Linux `hwmon` subsystem.

## Features

- **Temperature sensors** - `temp1_input`, `temp2_input` (millidegrees C)
- **Fan RPM** - `fan1_input`, `fan2_input`
- **Independent PWM control** - `pwm1`, `pwm2` (0-255), each fan independently addressable
- **Labels** - `temp1_label`, `fan1_label`, etc. for sensor identification
- **Keepalive** - polls the device every 10s so it stays in software mode; the firmware otherwise reverts fans to its default speed after ~30-60s of host silence
- **Self-healing** - if the device drops its software-mode session (seen after USB power-management events or idle), the driver re-enters software mode automatically on the next read or PWM write and restores commanded fan speeds
- **Hardware mode restore** - device returns to default behavior on driver unload
- Uses the CommanderCore protocol (same as [FanControl.CorsairLink](https://github.com/EvanMulawski/FanControl.CorsairLink))

## Requirements

- Linux kernel 6.8+ (tested; likely works on 5.15+)
- Build tools, DKMS and kernel headers:

  ```sh
  sudo apt install build-essential dkms linux-headers-$(uname -r)
  ```

Tested on Ubuntu 24.04 and Ubuntu 26.04.

## Install

Install with DKMS. It rebuilds the driver automatically for every new kernel,
so the driver keeps working across kernel updates.

```sh
git clone https://github.com/davemistretta/corsair-cduo-public.git
cd corsair-cduo-public
sudo dkms install .
sudo modprobe corsair-cduo
sensors
```

`sensors` should now list a `corsaircmdrduo` device. Nothing else is needed for
the driver to load at boot: the kernel loads it automatically when it sees the
device, so no `/etc/modules-load.d` entry is required.

Check the install at any time with:

```sh
dkms status corsair-cduo
```

It should show `installed` for the running kernel.

### Updating

```sh
git pull
sudo dkms remove corsair-cduo/1.0 --all
sudo dkms install .
sudo modprobe -r corsair-cduo && sudo modprobe corsair-cduo
```

### Uninstalling

```sh
sudo modprobe -r corsair-cduo
sudo dkms remove corsair-cduo/1.0 --all
sudo rm -rf /usr/src/corsair-cduo-1.0
```

### Trying it without installing

To build and load the driver once, without installing anything:

```sh
make
sudo insmod corsair-cduo.ko
```

This lasts until the next reboot. Run `make clean` afterwards if you then
install with DKMS from the same directory.

> **Avoid `sudo make install` for a permanent install.** It installs the module
> for the running kernel only, so the driver silently disappears at the next
> kernel update. If you installed that way before, remove the old copy before
> switching to DKMS:
>
> ```sh
> sudo rm /lib/modules/$(uname -r)/updates/corsair-cduo.ko
> sudo depmod -a
> ```

## sysfs Interface

The driver registers a `hwmon` device named `corsaircmdrduo`.

```sh
sensors
```

On **Ubuntu 26.04+ / lm-sensors ≥ 3.6.1**, PWM channels appear alongside fans and temperatures:

```
corsaircmdrduo-hid-3-1
Adapter: HID adapter
Fan 1:       1200 RPM
Fan 2:        900 RPM
Probe 1:      +35.0°C
Probe 2:      +38.0°C
pwm1:             50%
pwm2:            100%
```

On **Ubuntu 24.04 / lm-sensors ≤ 3.6.0**, only fans and temperatures are shown — older lm-sensors silently ignored PWM sysfs attributes.

The `pwm1`/`pwm2` values are the current duty cycle, displayed as a percentage. lm-sensors 3.6.1 (December 2023) added PWM sensor support; Ubuntu 26.04 ships with lm-sensors 3.6.2, which reads and displays these attributes. The driver's hwmon interface is correct — this is expected behavior, not a bug.

To suppress the PWM lines if the output is noisy, add an ignore directive to `/etc/sensors.d/corsair-cduo.conf`:

```ini
chip "corsaircmdrduo-*"
    ignore pwm1
    ignore pwm2
```

### Attributes

| Attribute | Access | Description |
|---|---|---|
| `temp1_input` | RO | Probe 1 temperature (millidegrees C) |
| `temp1_label` | RO | "Probe 1" |
| `temp2_input` | RO | Probe 2 temperature (millidegrees C) |
| `temp2_label` | RO | "Probe 2" |
| `fan1_input` | RO | Fan 1 speed (RPM) |
| `fan1_label` | RO | "Fan 1" |
| `fan2_input` | RO | Fan 2 speed (RPM) |
| `fan2_label` | RO | "Fan 2" |
| `pwm1` | RW | Fan 1 duty cycle (0-255) |
| `pwm2` | RW | Fan 2 duty cycle (0-255) |
| `firmware_version` | RO | Device firmware version string (e.g. `0.8.105`) |

### Setting Fan Speed

```sh
HWMON=$(grep -l corsaircmdrduo /sys/class/hwmon/hwmon*/name | sed 's|/name||')

# Set fan 1 to 50%
echo 128 | sudo tee $HWMON/pwm1

# Set fan 2 to 100%
echo 255 | sudo tee $HWMON/pwm2
```

The commanded speed holds until a new value is written or the driver is unloaded; the driver's 10-second keepalive keeps the device from timing out back to hardware mode. Reading `pwm1`/`pwm2` returns exactly the last value written — the device has no duty readback, so they read `0` until first written after the driver loads, regardless of how fast the fans are actually spinning.

## Troubleshooting

- **`sensors` shows no `corsaircmdrduo` device** — work down this list:
  - `dkms status corsair-cduo` — is the driver `installed` for the running kernel (`uname -r`)? If the kernel is missing, its headers were probably not installed when the kernel was; install `linux-headers-$(uname -r)` and run `sudo dkms autoinstall`.
  - `lsmod | grep corsair_cduo` — is it loaded? If not, `sudo modprobe corsair-cduo`.
  - `readlink -f /sys/bus/hid/devices/*1B1C:0C56*/driver` — one of the device's two interfaces should be bound to `corsair-cduo` (the other is left unbound). If it shows `hid-generic`, the driver is not loaded.
  - `sudo dmesg | grep -i cduo` — probe errors.
  - With Secure Boot enabled, the kernel rejects unsigned modules. DKMS on Ubuntu signs with a machine owner key (MOK) that must be enrolled; check with `mokutil --sb-state`.
- **`recovered from failed poll (...) by re-entering software mode` in dmesg** — informational, not an error. The device intermittently drops its software-mode session (typically after USB power-management events or long idle); the driver detected it, recovered automatically, and restored any commanded fan speeds.
- **`pwm1`/`pwm2` read 0 while fans spin** — expected until something writes them; see Usage.
- **`fan1_input` or `fan2_input` reads 0** — often "fan present, but no tach wire." The driver logs each channel's tach status to dmesg once at first use (`fanN: status 0x03 (tach signal present)` / `0x01 (no tach signal)`).

## Protocol

The device uses the CommanderCore protocol over 64-byte HID reports with a single communication handle (`0xfc`). Sensor reads and fan writes use an endpoint open/close cycle:

```
Close endpoint:  [08 05 01 fc]
Open endpoint:   [08 0d fc <endpoint>]
Read endpoint:   [08 08 fc]
Write endpoint:  [08 06 fc <len_lo> <len_hi> 00 00 <dtype> <data...>]
```

| Endpoint | Dtype | Direction | Description |
|---|---|---|---|
| `0x17` | `0x06` | Read | Fan RPM |
| `0x21` | `0x10` | Read | Temperature |
| `0x18` | `0x07` | Write | Fan speed (per-channel, 0-100%) |

See [NOTES.md](NOTES.md) for detailed protocol documentation.

## Fan Control

The driver only provides raw sensor access and PWM control. To implement automatic fan curves, use any Linux fan control tool that supports hwmon:

- [fancontrol](https://wiki.archlinux.org/title/Fan_speed_control#fancontrol) (lm-sensors)
- [thinkfan](https://github.com/vmatare/thinkfan)
- Custom scripts reading sysfs and writing to `pwm1`/`pwm2`

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

Originally inspired by [MisterZ42/corsair-cpro](https://github.com/MisterZ42/corsair-cpro). Protocol details informed by [FanControl.CorsairLink](https://github.com/EvanMulawski/FanControl.CorsairLink).
