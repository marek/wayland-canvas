"""NetworkManager stand-in for the bar scenario (system bus).

dbusmock's networkmanager template already covers what Quickshell 0.3.1's
Networking service reads (manager, devices, access points, settings,
active connections); this wrapper adds the methods it lacks (GetAllDevices
on the manager, Disconnect on the device) and loads
the scenario's fixed Wi-Fi world on one connected device:

    CafeGuest    secured, unknown, strongest — the typing check's network
    HomeNet      secured, known, connected
    LibraryFree  open, unknown

The panel sorts by signal strength, so CafeGuest is the first row.
"""

# noqa: F401,F403 — the star import is the point: AddTemplate wires every
# dbusmock method of the wrapped template onto the mock object.
from dbusmock.templates.networkmanager import *  # noqa: F401,F403

import dbus  # noqa: F401  (template method bodies evaluate against it)
import dbusmock
import dbusmock.templates.networkmanager as nm

# the pair cipher Quickshell's security check requires for a Wpa2Psk
# network: without it, connectWithPsk refuses the network
CCMP = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_PAIR_CCMP
SECURED = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_KEY_MGMT_PSK | CCMP
# the template maps an AP's flags to connection settings through a
# one-entry table; teach it the flag combination above (PSK + pair CCMP)
nm.NM80211ApSecurityFlags.NAME_MAP[SECURED] = {"key-mgmt": "wpa-psk", "auth-alg": "open"}
OPEN = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_NONE


def load(mock, parameters):
    nm.load(mock, parameters)

    # Quickshell lists devices through GetAllDevices; dbusmock ships only
    # GetDevices. The manager object, not the template root, serves it.
    dbusmock.get_object(nm.MANAGER_OBJ).AddMethods(
        nm.MANAGER_IFACE,
        [("GetAllDevices", "", "ao", 'ret = [k for k in objects.keys() if "/Devices" in k]')],
    )

    device = mock.AddWiFiDevice("wifi0", "wlan0", nm.DeviceState.ACTIVATED)

    # Quickshell's network disconnect calls Disconnect on the device object;
    # dbusmock ships no such method, so wire one that drops the active
    # connection the way the panel expects to see.
    dbusmock.get_object(nm.MANAGER_OBJ + "/Devices/wifi0").AddMethods(
        nm.DEVICE_IFACE,
        [(
            "Disconnect",
            "",
            "",
            "self.Set('" + nm.DEVICE_IFACE + "', 'ActiveConnection', dbus.ObjectPath('/'))\n"
            "self.Set('" + nm.DEVICE_IFACE + "', 'State', dbus.UInt32(30))",
        )],
    )

    # A successful activation would move the device's connection from
    # HomeNet to CafeGuest and reshuffle the panel's rows; the scenario
    # needs the recorded call, not the state change, so the activation
    # answers with paths but leaves the connected network alone.
    dbusmock.get_object(nm.MANAGER_OBJ).add_and_activate_connection = (
        lambda self_, settings, dev, ap: (
            dbus.ObjectPath("/org/freedesktop/NetworkManager/ActiveConnection/cafe"),
            dbus.ObjectPath("/org/freedesktop/NetworkManager/ActiveConnection/cafe"),
        )
    )
    # HomeNet's flags stay plain PSK: AddWiFiConnection maps them through a
    # one-entry table. CafeGuest — the typing flow's target — carries the
    # pair cipher too, or Quickshell refuses it a PSK.
    home_ap = mock.AddAccessPoint(device, "home", "HomeNet", "44:55:66:77:88:99", 2, 2437, 54000, 70, nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_KEY_MGMT_PSK)
    mock.AddAccessPoint(device, "cafe", "CafeGuest", "66:77:88:99:aa:bb", 2, 2437, 54000, 90, SECURED)
    mock.AddAccessPoint(device, "library", "LibraryFree", "22:33:44:55:66:77", 2, 2462, 54000, 50, OPEN)
    home_conn = mock.AddWiFiConnection(device, "home", "HomeNet", "wpa-psk")
    mock.AddActiveConnection(
        [device],
        home_conn,
        home_ap,
        "home",
        nm.NMActiveConnectionState.NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
    )
