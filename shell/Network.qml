import Quickshell.Networking
import QtQuick

// Network: connection type and Wi-Fi strength. Left opens nmtui (the
// NetworkManager terminal UI).
BarButton {
	id: root

	readonly property var device: currentDevice()
	readonly property var wifi: device != null && device.type === NetworkDeviceType.Wifi
		? device
		: null
	readonly property int strength: connectedStrength()

	icon: true
	visible: device != null
	text: device == null || device.connected
		? (wifi != null ? wifiGlyph(strength) : (device.type === NetworkDeviceType.Ethernet
			? "󰈀" : "󰤮"))
		: "󰤮"

	Process {
		id: terminal
		command: ["/bin/sh", "-c",
			"if command -v xdg-terminal-exec >/dev/null 2>&1; " +
			"then exec xdg-terminal-exec -- \"$@\"; else exec foot -e \"$@\"; fi",
			"tui", "nmtui"]
	}

	onPressed: function(button) {
		if (button === Qt.LeftButton) {
			terminal.running = true
		}
	}

	// The device to describe: a connected one, else the first present.
	function currentDevice() {
		var devices = Networking.devices ? Networking.devices.values : []
		var first = null
		for (var i = 0; i < devices.length; i++) {
			if (devices[i].connected) {
				return devices[i]
			}
			if (first == null && devices[i].state !== NetworkDeviceState.Unavailable) {
				first = devices[i]
			}
		}
		return first
	}

	function connectedStrength() {
		if (wifi == null || !wifi.networks) {
			return 0
		}
		var networks = wifi.networks.values
		for (var i = 0; i < networks.length; i++) {
			if (networks[i].connected) {
				return networks[i].signalStrength
			}
		}
		return 0
	}

	function wifiGlyph(strength) {
		var icons = ["󰤯", "󰤟", "󰤢", "󰤥", "󰤨"]
		var index = Math.max(0, Math.min(4, Math.ceil(strength / 20) - 1))
		return icons[index]
	}
}
