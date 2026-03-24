# corsair-cduo

Linux kernel HID driver for the **Corsair Commander Duo** (USB VID `0x1b1c`, PID `0x0c56`).

Exposes fan RPM, temperature sensors, and per-channel PWM fan speed control via the standard Linux `hwmon` subsystem.

## Features

- **Temperature sensors** - `temp1_input`, `temp2_input` (millidegrees C)
- **Fan RPM** - `fan1_input`, `fan2_input`
- **Independent PWM control** - `pwm1`, `pwm2` (0-255), each fan independently addressable
- **Labels** - `temp1_label`, `fan1_label`, etc. for sensor identification
- **Speed latching** - device holds commanded speed; no background polling required
- **Hardware mode restore** - device returns to default behavior on driver unload
- Uses the CommanderCore protocol (same as [FanControl.CorsairLink](https://github.com/EvanMulawski/FanControl.CorsairLink))

## Requirements

- Linux kernel 6.8+ (tested; likely works on 5.15+)
- Kernel headers: `linux-headers-$(uname -r)`

## Build and Install

```sh
make
sudo insmod corsair-cduo.ko
sensors
```

To install permanently:

```sh
sudo make install
sudo depmod -a
echo "corsair_cduo" | sudo tee /etc/modules-load.d/corsair-cduo.conf
```

Or use DKMS:

```sh
sudo cp -r . /usr/src/corsair-cduo-1.0/
cat <<EOF | sudo tee /usr/src/corsair-cduo-1.0/dkms.conf
PACKAGE_NAME="corsair-cduo"
PACKAGE_VERSION="1.0"
BUILT_MODULE_NAME[0]="corsair-cduo"
DEST_MODULE_LOCATION[0]="/kernel/drivers/hid/"
AUTOINSTALL="yes"
EOF
sudo dkms install corsair-cduo/1.0
```

## sysfs Interface

The driver registers a `hwmon` device named `corsaircmdrduo`.

```sh
sensors
```

```
corsaircmdrduo-hid-3-1
Adapter: HID adapter
Fan 1:       1200 RPM
Fan 2:        900 RPM
Probe 1:      +35.0°C
Probe 2:      +38.0°C
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

### Setting Fan Speed

```sh
HWMON=$(grep -l corsaircmdrduo /sys/class/hwmon/hwmon*/name | sed 's|/name||')

# Set fan 1 to 50%
echo 128 | sudo tee $HWMON/pwm1

# Set fan 2 to 100%
echo 255 | sudo tee $HWMON/pwm2
```

The device latches the commanded speed. It will hold the target until a new value is written or the driver is unloaded.

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
