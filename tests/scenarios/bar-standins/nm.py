"""NetworkManager stand-in for the bar scenario (system bus).

dbusmock's networkmanager template already covers what Quickshell 0.3.1's
Networking service reads (manager, devices, access points, settings,
active connections); this wrapper adds the methods it lacks (GetAllDevices
on the manager, Disconnect on the device) and loads the scenario's fixed
Wi-Fi world on one connected device:

    CafeGuest    secured, unknown, strongest — the password join's network
    CorpNet      enterprise (802.1X), unknown — the identity join's network
    HomeNet      secured, known, connected
    LibraryFree  open, unknown — the click-join's network
    OldCafe      secured, known, not connected — the forget action's network;
                 its saved profile is named "Old Cafe profile", not by SSID

The panel sorts connected first, then known, then signal, so the rows read
HomeNet, OldCafe, CafeGuest, CorpNet, LibraryFree. Two more networks join
the world on demand, when the scenario calls RoamColon or RoamSemicolon:
`cafe:5G` and `Lounge;Guest`, each connected as the machine roams to it;
RoamHome moves it back to HomeNet.
"""

# noqa: F401,F403 — the star import is the point: AddTemplate wires every
# dbusmock method of the wrapped template onto the mock object.
from dbusmock.templates.networkmanager import *  # noqa: F401,F403

import dbus  # noqa: F401  (template method bodies evaluate against it)
import dbusmock
import dbusmock.templates.networkmanager as nm

# the ciphers Quickshell's security check requires: without the pair
# cipher it refuses a PSK network's connectWithPsk, and without the group
# cipher it will not classify the enterprise network as Wpa2Eap at all
CCMP = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_PAIR_CCMP
GROUP = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_GROUP_CCMP
SECURED = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_KEY_MGMT_PSK | CCMP
# the template maps an AP's flags to connection settings through a
# one-entry table; teach it the flag combinations above (PSK + pair CCMP,
# and 802.1X + both CCMP ciphers for the enterprise network)
nm.NM80211ApSecurityFlags.NAME_MAP[SECURED] = {"key-mgmt": "wpa-psk", "auth-alg": "open"}
ENTERPRISE = (nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_KEY_MGMT_802_1X
    | CCMP | GROUP)
nm.NM80211ApSecurityFlags.NAME_MAP[ENTERPRISE] = {"key-mgmt": "wpa-eap"}
OPEN = nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_NONE
# the interface the scenario's roams are called on
ROAM_IFACE = "org.waylandcanvas.barstandins.Wifi"


def load(mock, parameters):
    nm.load(mock, parameters)

    # Quickshell lists devices through GetAllDevices; dbusmock ships only
    # GetDevices. The manager object, not the template root, serves it.
    dbusmock.get_object(nm.MANAGER_OBJ).AddMethods(
        nm.MANAGER_IFACE,
        [("GetAllDevices", "", "ao", 'ret = [k for k in objects.keys() if "/Devices" in k]')],
    )

    device = mock.AddWiFiDevice("wifi0", "wlan0", nm.DeviceState.ACTIVATED)
    device_path = nm.MANAGER_OBJ + "/Devices/wifi0"
    manager = dbusmock.get_object(nm.MANAGER_OBJ)

    # Quickshell's network disconnect calls Disconnect on the device object;
    # dbusmock ships no such method. The override drops the active
    # connection the way the real daemon would — device idle, the active
    # connection object gone — so the panel's later reads (a reload reads
    # everything again) keep seeing a disconnected world.
    def disconnect(self_):
        active = self_.Get(nm.DEVICE_IFACE, "ActiveConnection")
        if str(active) == "/":
            return
        manager.RemoveActiveConnection(device_path, active)

    # AddMethod with a function as its code is the one override dbusmock
    # honours: the method is registered for dispatch and the call is
    # logged, while a plain attribute assignment changes neither.
    dbusmock.get_object(device_path).AddMethod(
        nm.DEVICE_IFACE, "Disconnect", "", "", disconnect)

    # Activating a known connection moves the device to activated with the
    # connection active on it, as the real daemon would; the template's
    # ActivateConnection only registers the active-connection object and
    # leaves the device untouched, so the panel would never see the join.
    def activate(self_, conn, dev, ap):
        path = nm.activate_connection(self_, conn, dev, ap)
        dev_obj = dbusmock.get_object(device_path)
        old_state = dev_obj.Get(nm.DEVICE_IFACE, "State")
        dev_obj.Set(nm.DEVICE_IFACE, "ActiveConnection", path)
        dev_obj.Set(nm.DEVICE_IFACE, "State",
            dbus.UInt32(nm.DeviceState.ACTIVATED))
        dev_obj.Set(nm.DEVICE_IFACE, "StateReason",
            (dbus.UInt32(nm.DeviceState.ACTIVATED), dbus.UInt32(0)))
        dev_obj.EmitSignal(nm.DEVICE_IFACE, "StateChanged", "uuu",
            [dbus.UInt32(nm.DeviceState.ACTIVATED), old_state,
             dbus.UInt32(1)])
        return path

    # A successful join of an unknown network would move the device's
    # connection from HomeNet to the joined network and reshuffle the
    # panel's rows; the scenario needs the recorded call, not the state
    # change, so this activation answers with paths but leaves the
    # connected network alone.
    def add_and_activate(self_, settings, dev, ap):
        return (
            dbus.ObjectPath("/org/freedesktop/NetworkManager/ActiveConnection/joined"),
            dbus.ObjectPath("/org/freedesktop/NetworkManager/ActiveConnection/joined"),
        )

    manager.AddMethod(nm.MANAGER_IFACE, "ActivateConnection",
        "ooo", "o", activate)
    manager.AddMethod(nm.MANAGER_IFACE, "AddAndActivateConnection",
        "a{sa{sv}}oo", "oo", add_and_activate)

    # HomeNet's flags stay plain PSK: AddWiFiConnection maps them through a
    # one-entry table. The joined networks — the password flow's targets —
    # carry the pair cipher too, or Quickshell refuses them a PSK.
    home_ap = mock.AddAccessPoint(device, "home", "HomeNet", "44:55:66:77:88:99", 2, 2437, 54000, 70, nm.NM80211ApSecurityFlags.NM_802_11_AP_SEC_KEY_MGMT_PSK)
    mock.AddAccessPoint(device, "cafe", "CafeGuest", "66:77:88:99:aa:bb", 2, 2437, 54000, 90, SECURED)
    mock.AddAccessPoint(device, "corp", "CorpNet", "77:88:99:aa:bb:cc", 2, 5240, 54000, 80, ENTERPRISE)
    mock.AddAccessPoint(device, "library", "LibraryFree", "22:33:44:55:66:77", 2, 2462, 54000, 50, OPEN)
    mock.AddAccessPoint(device, "oldcafe", "OldCafe", "33:44:55:66:77:88", 2, 2462, 54000, 40, SECURED)
    home_conn = mock.AddWiFiConnection(device, "home", "HomeNet", "wpa-psk")
    # OldCafe's saved profile is not named after its SSID, as a profile
    # made by hand or by another tool is not: forgetting the network has to
    # find the profile by what it holds, never by the SSID as a name.
    oldcafe_conn = mock.AddWiFiConnection(device, "oldcafe", "OldCafe", "wpa-psk")
    dbusmock.get_object(oldcafe_conn).settings["connection"]["id"] = "Old Cafe profile"
    mock.AddActiveConnection(
        [device],
        home_conn,
        home_ap,
        "home",
        nm.NMActiveConnectionState.NM_ACTIVE_CONNECTION_STATE_ACTIVATED,
    )

    # The machine moves to a network the world did not list at the start;
    # the scenario asks for each move over D-Bus. The access point and its
    # saved profile appear, the old connection drops and the new one comes
    # up, as when the daemon roams on its own. Their SSIDs are the two
    # characters that nmcli's terse mode and the WIFI: payload both treat
    # specially.
    def roam(ap_name, ssid, hw_address, frequency):
        disconnect(dbusmock.get_object(device_path))
        ap = mock.AddAccessPoint(device, ap_name, ssid, hw_address, 2,
            frequency, 54000, 85, SECURED)
        conn = mock.AddWiFiConnection(device, ap_name, ssid, "wpa-psk")
        activate(manager, conn, device_path, ap)

    def roam_colon(self_):
        roam("colon", "cafe:5G", "88:99:aa:bb:cc:01", 5180)

    def roam_semicolon(self_):
        roam("semicolon", "Lounge;Guest", "88:99:aa:bb:cc:02", 2437)

    def roam_home(self_):
        disconnect(dbusmock.get_object(device_path))
        activate(manager, home_conn, device_path, home_ap)

    manager.AddMethod(ROAM_IFACE, "RoamColon", "", "", roam_colon)
    manager.AddMethod(ROAM_IFACE, "RoamSemicolon", "", "", roam_semicolon)
    manager.AddMethod(ROAM_IFACE, "RoamHome", "", "", roam_home)
