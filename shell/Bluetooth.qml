import Quickshell.Bluetooth
import QtQuick
import "."

// Bluetooth: radio state, connected mark while a device holds a link. Left
// opens the panel (power, scan, devices); right still flips the radio.
// Hidden without an adapter.
BarButton {
	id: root

	readonly property var adapter: Bluetooth.defaultAdapter
	readonly property bool radioOn: adapter != null && adapter.enabled
	readonly property bool linked: adapter != null && anyConnected()

	icon: true
	visible: adapter != null
	text: !radioOn ? "󰂲" : (linked ? "󰂱" : "󰂯")

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			adapter.enabled = !adapter.enabled
		} else {
			Panels.toggle(panel)
		}
	}

	BluetoothPanel {
		id: panel
		theme: root.theme
		anchorItem: root
		model: root
	}

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
