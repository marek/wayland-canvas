#!/usr/bin/env python3
"""Stand-in services for the bar scenario, on private buses only.

The bar scenario starts this launcher after bringing up its private system
and session buses (DBUS_SYSTEM_BUS_ADDRESS / DBUS_SESSION_BUS_ADDRESS, both
pointing at sockets under the repo's tmp/) and before the compositor spawns
the bar, so the bar reads a fixed world instead of the machine's services:

  system bus: NetworkManager, BlueZ and UPower (bar-standins/*.py wrapper
              templates around dbusmock's) and power-profiles-daemon
              (dbusmock's own upower_power_profiles_daemon template)
  session bus: one StatusNotifierItem whose com.canonical.dbusmenu menu has
               a few items, registered with the bar's own
               StatusNotifierWatcher once the bar starts it

Prints READY on stdout once the four system-bus names are owned; the tray
item retries registration until the bar's watcher appears. Exits when
either bus goes away or a mock dies. Refuses to start unless both bus
addresses are set, so a stand-in never lands on the machine's buses.
"""

import ctypes
import os
import signal
import subprocess
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

HERE = os.path.dirname(os.path.abspath(__file__))
SYSTEM_MOCKS = [
    os.path.join(HERE, "bar-standins", "nm.py"),
    os.path.join(HERE, "bar-standins", "bluez.py"),
    os.path.join(HERE, "bar-standins", "upower.py"),
    "upower_power_profiles_daemon",  # shipped by dbusmock
]
SYSTEM_NAMES = [
    "org.freedesktop.NetworkManager",
    "org.bluez",
    "org.freedesktop.UPower",
    "org.freedesktop.UPower.PowerProfiles",
]
LOG_DIR = os.environ.get("CANVAS_BAR_DIR") or os.path.join(
    os.environ.get("CANVAS_TMP", "tmp"), "scenarios", "bar")
TRAY_NAME = "org.waylandcanvas.barstandins.Tray1"
WATCHER_NAME = "org.kde.StatusNotifierWatcher"
WATCHER_PATH = "/StatusNotifierWatcher"
TRAY_PATH = "/MenuBar"


def die(message):
    print(f"bar-standins: {message}", file=sys.stderr, flush=True)
    sys.exit(1)


def set_pdeathsig():
    # a mock must never outlive the launcher
    try:
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except OSError:
        pass


def solid_pixmap(size):

    return dbus.Array(
        [
            dbus.Struct(
                (
                    dbus.UInt32(size),
                    dbus.UInt32(size),
                    dbus.Array([dbus.Byte(255)] * size * size * 4, signature="y"),
                ),
                signature=None,
            )
        ],
        signature="(iiay)",
    )


class TrayItem(dbus.service.Object):
    """One StatusNotifierItem with its dbusmenu on the same object."""

    def __init__(self, bus):
        self.items = {
            0: {"children-display": "submenu", "label": ""},
            1: {"label": "Synchronize now", "enabled": True},
            2: {"label": "Auto-synchronize", "toggle-type": "checkmark", "toggle-state": 1},
            3: {"type": "separator", "label": ""},
            4: {"label": "Report a problem", "enabled": False},
            5: {"label": "About", "enabled": True},
        }
        self.children = {0: [1, 2, 3, 4, 5]}
        super().__init__(bus, TRAY_PATH)

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.GetAll(interface)[name]

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface == "org.kde.StatusNotifierItem":
            return {
                "Category": dbus.String("SystemServices"),
                "Id": dbus.String("bar-standins-tray"),
                "Title": dbus.String("Stand-in tray"),
                "Status": dbus.String("Active"),
                "IconName": dbus.String(""),
                "IconPixmap": solid_pixmap(16),
                "OverlayIconName": dbus.String(""),
                "AttentionIconName": dbus.String(""),
                "Menu": dbus.ObjectPath(TRAY_PATH),
                "ItemIsMenu": dbus.Boolean(False),
            }
        if interface == "com.canonical.dbusmenu":
            return {"Version": dbus.UInt32(2)}
        return {}

    @dbus.service.method("org.kde.StatusNotifierItem", in_signature="ii", out_signature="")
    def Activate(self, _x, _y):
        pass

    @dbus.service.method("org.kde.StatusNotifierItem", in_signature="ii", out_signature="")
    def SecondaryActivate(self, _x, _y):
        pass

    @dbus.service.method("org.kde.StatusNotifierItem", in_signature="is", out_signature="")
    def Scroll(self, _delta, _orientation):
        pass

    def layout(self, item):
        props = dbus.Dictionary(
            {
                key: dbus.String(value) if isinstance(value, str) else value
                for key, value in self.items[item].items()
            },
            signature="sv",
        )
        kids = dbus.Array([self.layout(child) for child in self.children.get(item, [])], signature="v")
        return dbus.Struct((dbus.Int32(item), props, kids), signature=None)

    @dbus.service.method("com.canonical.dbusmenu", in_signature="iias", out_signature="u(ia{sv}av)")
    def GetLayout(self, _parent, _depth, _names):
        return (dbus.UInt32(0), self.layout(0))

    @dbus.service.method("com.canonical.dbusmenu", in_signature="aias", out_signature="a(ia{sv})")
    def GetGroupProperties(self, ids, _names):
        return [
            dbus.Struct(
                (
                    dbus.Int32(item),
                    dbus.Dictionary(
                        {key: dbus.String(value) for key, value in props.items() if key != "type"},
                        signature="sv",
                    ),
                ),
                signature=None,
            )
            for item, props in self.items.items()
            if item in ids
        ]

    @dbus.service.method("com.canonical.dbusmenu", in_signature="i", out_signature="b")
    def AboutToShow(self, _item):
        return False

    @dbus.service.method("com.canonical.dbusmenu", in_signature="isvu", out_signature="")
    def Event(self, _item, _event, _data, _timestamp):
        pass


class Standins:
    def __init__(self):
        for variable in ("DBUS_SYSTEM_BUS_ADDRESS", "DBUS_SESSION_BUS_ADDRESS"):
            if not os.environ.get(variable):
                die(f"{variable} is not set: refusing to touch the machine's buses")
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.mocks = []
        self.tray_name = None
        self.loop = GLib.MainLoop()
        self.system_bus = dbus.SystemBus()
        self.session_bus = dbus.SessionBus()
        for bus in (self.system_bus, self.session_bus):
            # the constructors above raise when their bus is not reachable
            bus.add_signal_receiver(
                self.loop.quit,
                signal_name="Disconnected",
                path="/org/freedesktop/DBus/Local",
                dbus_interface="org.freedesktop.DBus.Local",
            )

    def start(self):
        # the mock logs go to a file, not to this launcher's stdout: that
        # stdout is the readiness pipe to the scenario and must never block
        # on unread bulk output (stderr too, so tracebacks survive)
        log = open(os.path.join(LOG_DIR, "dbusmock.log"), "wb")
        for template in SYSTEM_MOCKS:
            mock = subprocess.Popen(
                [sys.executable, "-m", "dbusmock", "--template", template],
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                preexec_fn=set_pdeathsig,
            )
            self.mocks.append(mock)
            GLib.child_watch_add(mock.pid, self.on_mock_exit)
        if not self.wait_for_names():
            die(f"the stand-in services did not appear on the private system bus "
                f"(saw: {', '.join(sorted(self.own_names()))})")

        # keep the reference: the name is released when the BusName object dies
        self.tray_name = dbus.service.BusName(TRAY_NAME, self.session_bus)
        TrayItem(self.session_bus)
        GLib.timeout_add(500, self.register_tray)
        print("READY", flush=True)
        self.loop.run()

    def own_names(self):
        interface = dbus.Interface(
            self.system_bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus"),
            "org.freedesktop.DBus",
        )
        return [n for n in interface.ListNames() if n != "org.freedesktop.DBus"]

    def wait_for_names(self):
        interface = dbus.Interface(
            self.system_bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus"),
            "org.freedesktop.DBus",
        )
        for _ in range(100):  # 100 x 0.2 s
            if set(SYSTEM_NAMES) <= set(interface.ListNames()):
                return True
            time.sleep(0.2)
        return False

    def register_tray(self):
        # the bar hosts the watcher; it appears when the bar starts
        try:
            watcher = dbus.Interface(
                self.session_bus.get_object(WATCHER_NAME, WATCHER_PATH), WATCHER_NAME
            )
            watcher.RegisterStatusNotifierItem(f"{TRAY_NAME}{TRAY_PATH}")
            return False  # registered: stop retrying
        except dbus.exceptions.DBusException:
            return True

    def on_mock_exit(self, _pid, _status):
        die("a stand-in service exited early")


def main():
    standins = Standins()

    def terminate(_signum, _frame):
        for mock in standins.mocks:
            mock.terminate()
        standins.loop.quit()

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    standins.start()


if __name__ == "__main__":
    main()
