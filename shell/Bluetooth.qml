import Quickshell.Bluetooth
import Quickshell.Io
import QtQuick

// Bluetooth: radio state, connected mark while a device holds a link.
// Left opens bluetoothctl in the terminal, right toggles the radio.
// Hidden without an adapter.
BarButton {
	id: root

	readonly property var adapter: Bluetooth.defaultAdapter
	readonly property bool radioOn: adapter != null && adapter.enabled
	readonly property bool linked: adapter != null && anyConnected()

	icon: true
	visible: adapter != null
	text: !radioOn ? "󰂲" : (linked ? "󰂱" : "󰂯")

	Process {
		id: terminal
		command: ["/bin/sh", "-c",
			"if command -v xdg-terminal-exec >/dev/null 2>&1; " +
			"then exec xdg-terminal-exec -- \"$@\"; else exec foot -e \"$@\"; fi",
			"tui", "bluetoothctl"]
	}

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			adapter.enabled = !adapter.enabled
		} else {
			terminal.running = true
		}
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
