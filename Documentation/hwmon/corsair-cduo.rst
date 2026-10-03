.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver corsair-cduo
==========================

Supported devices:

  * Corsair Commander Duo

    USB VID: 0x1b1c, PID: 0x0c56

Author: David Mistretta

Description
-----------

This driver provides hwmon support for the Corsair Commander Duo, a USB
fan and temperature controller. The device has two fan headers and two
temperature probe connectors.

The driver communicates with the device using the CommanderCore protocol
over 64-byte HID reports. A single handle (0xfc) is used with
endpoint-based open/close cycles to read sensors and write fan speeds.

The device drops out of software mode and reverts the fans to its hardware
default after roughly 30-60 seconds without host traffic. Once initialized,
the driver polls every 10 seconds to keep the session (and commanded fan
speeds) alive.

Usage Notes
-----------

The driver uses lazy initialization. The device is not polled until the
first sensor read (e.g., via ``sensors`` or reading sysfs attributes).
The keepalive poll starts after that first successful initialization.

Fan speed is set using standard hwmon PWM attributes (``pwm1``, ``pwm2``)
with values 0-255. Each fan channel is independently controllable.
The device has no duty readback: ``pwmN`` returns the last value written,
or 0 if it has not been written since the driver loaded.

When the driver is unloaded, the device is returned to hardware mode.

Sysfs attributes
----------------

======================= ======= ==========================================
Attribute               Perm    Description
======================= ======= ==========================================
temp1_input             RO      Temperature probe 1 (millidegrees C)
temp1_label             RO      "Probe 1"
temp2_input             RO      Temperature probe 2 (millidegrees C)
temp2_label             RO      "Probe 2"
fan1_input              RO      Fan 1 speed (RPM)
fan1_label              RO      "Fan 1"
fan2_input              RO      Fan 2 speed (RPM)
fan2_label              RO      "Fan 2"
pwm1                    RW      Fan 1 duty cycle (0-255)
pwm2                    RW      Fan 2 duty cycle (0-255)
======================= ======= ==========================================
