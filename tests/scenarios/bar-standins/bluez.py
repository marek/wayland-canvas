"""BlueZ stand-in for the bar scenario (system bus).

dbusmock's bluez5 template provides the ObjectManager, adapter and device
interfaces Quickshell 0.3.1's Bluetooth service reads; this wrapper loads
the scenario's fixed world: one powered adapter with a paired device and
an unpaired found device.
"""

from dbusmock.templates.bluez5 import *  # noqa: F401,F403

import dbus  # noqa: F401
import dbusmock.mockobject
import dbusmock.templates.bluez5 as bluez


def load(mock, parameters):
    # the bluez5 convenience methods reach the root object through the
    # global object table, which the mock runner fills only after load
    dbusmock.mockobject.objects.setdefault("/", mock)
    bluez.load(mock, parameters)

    bluez.AddAdapter(mock, "hci0", "bar-scenario")
    bluez.AddDevice(mock, "hci0", "11:22:33:44:55:66", "Scenario Headphones")
    bluez.PairDevice(mock, "hci0", "11:22:33:44:55:66")

    # unpaired, as a scan would find it
    bluez.AddDevice(mock, "hci0", "aa:bb:cc:dd:ee:ff", "Scenario Speaker")
