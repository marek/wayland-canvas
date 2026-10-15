"""NetworkManager stand-in for the bar scenario (system bus).

dbusmock's networkmanager template already covers what Quickshell 0.3.1's
Networking service reads (manager, devices, access points, settings,
active connections); this wrapper adds the one method it lacks and loads
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

PSK = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_KEY_MGMT_PSK
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
    home_ap = mock.AddAccessPoint(device, "home", "HomeNet", "44:55:66:77:88:99", 2, 2437, 54000, 70, PSK)
    mock.AddAccessPoint(device, "cafe", "CafeGuest", "66:77:88:99:aa:bb", 2, 2437, 54000, 90, PSK)
    mock.AddAccessPoint(device, "library", "LibraryFree", "22:33:44:55:66:77", 2, 2462, 54000, 50, OPEN)
    home_conn = mock.AddWiFiConnection(device, "home", "HomeNet", "wpa-psk")
    mock.AddActiveConnection(
        [device],
        home_conn,
        home_ap,
        "home",
        nm.NMActiveConnectionState.NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
    )
