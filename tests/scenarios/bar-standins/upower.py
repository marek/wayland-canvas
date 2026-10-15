"""UPower stand-in for the bar scenario (system bus).

dbusmock's upower template provides the service and device objects
Quickshell 0.3.1's UPower service reads (EnumerateDevices, GetDisplayDevice
and the device properties); this wrapper loads the scenario's fixed world:
one discharging battery, also set up as the display device.
"""

from dbusmock.templates.upower import *  # noqa: F401,F403

import dbus  # noqa: F401
import dbusmock.templates.upower as upower

# UP_DEVICE_STATE_DISCHARGING, UP_DEVICE_KIND_BATTERY
DISCHARGING = 2
BATTERY = 2


def load(mock, parameters):
    upower.load(mock, parameters)

    upower.AddDischargingBattery(mock, "bat0", "Scenario Battery", 55.0, 7200)
    upower.SetupDisplayDevice(
        mock,
        BATTERY,
        DISCHARGING,
        55.0,
        55.0,
        100.0,
        12.5,
        7200,
        0,
        True,
        "battery-full",
        1,
    )
