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
    # the template's Connect/Disconnect flip a private attribute and signal
    # the change, but leave the D-Bus property stale; keep the property in
    # step so the scenario can read the state back off the bus
    orig_connect, orig_disconnect = bluez.Connect, bluez.Disconnect

    def connect_then_prop(device):
        orig_connect(device)
        device.props[bluez.DEVICE_IFACE]["Connected"] = dbus.Boolean(True)

    def disconnect_then_prop(device):
        orig_disconnect(device)
        device.props[bluez.DEVICE_IFACE]["Connected"] = dbus.Boolean(False)

    bluez.Connect, bluez.Disconnect = connect_then_prop, disconnect_then_prop

    # the bluez5 convenience methods reach the root object through the
    # global object table, which the mock runner fills only after load
    dbusmock.mockobject.objects.setdefault("/", mock)
    bluez.load(mock, parameters)
    bluez.AddAdapter(mock, "hci0", "bar-scenario")
    bluez.AddDevice(mock, "hci0", "11:22:33:44:55:66", "Scenario Headphones")
    bluez.PairDevice(mock, "hci0", "11:22:33:44:55:66")

    # unpaired, as a scan would find it
    bluez.AddDevice(mock, "hci0", "aa:bb:cc:dd:ee:ff", "Scenario Speaker")
