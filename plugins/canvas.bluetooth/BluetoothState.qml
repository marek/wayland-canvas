pragma Singleton

import Quickshell.Bluetooth
import QtQuick

// What the Bluetooth widget on every bar and the Bluetooth panel share: the
// adapter, whether its radio is on and whether a device holds a link.
QtObject {
	readonly property var adapter: Bluetooth.defaultAdapter
	readonly property bool radioOn: adapter != null && adapter.enabled
	readonly property bool linked: adapter != null && anyConnected()

	function anyConnected() {
		var devices = adapter.devices ? adapter.devices.values : []
		for (var i = 0; i < devices.length; i++) {
			if (devices[i].connected) {
				return true
			}
		}
		return false
	}
}
