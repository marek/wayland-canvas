import Quickshell.Io
import Quickshell.Networking
import QtQuick
import "."

// Network: connection type and Wi-Fi strength; the click opens the panel
// (Wi-Fi switch, networks, connect and disconnect).
BarButton {
	id: root

	readonly property var device: currentDevice()
	readonly property var wifi: device != null && device.type === DeviceType.Wifi
		? device
		: null
	readonly property int strength: connectedStrength()

	// The visible networks, strongest first — the panel's list.
	readonly property var networks: {
		var list = wifi != null && wifi.networks ? wifi.networks.values.slice() : []
		list.sort(function(a, b) {
			return b.signalStrength - a.signalStrength
		})
		return list
	}

	icon: true
	visible: device != null
	text: {
		if (device == null || !device.connected) {
			return "󰤮"
		}
		if (wifi != null) {
			return wifiGlyph(strength)
		}
		return device.type === DeviceType.Ethernet ? "󰈀" : "󰤮"
	}

	onPressed: function(button) {
		if (button === Qt.LeftButton) {
			Panels.toggle(panel)
		}
	}

	NetworkPanel {
		id: panel
		theme: root.theme
		anchorItem: root
		model: root
	}

	// The device to describe: a connected one, else the first (the backend
	// only lists devices the system actually has).
	function currentDevice() {
		var devices = Networking.devices ? Networking.devices.values : []
		for (var i = 0; i < devices.length; i++) {
			if (devices[i].connected) {
				return devices[i]
			}
		}
		return devices.length > 0 ? devices[0] : null
	}

	// A network's signal in percent; the backend reports 0 to 1.
	function percent(network) {
		return Math.round(network.signalStrength * 100)
	}

	function connectedStrength() {
		if (wifi == null || !wifi.networks) {
			return 0
		}
		var networks = wifi.networks.values
		for (var i = 0; i < networks.length; i++) {
			if (networks[i].connected) {
				return percent(networks[i])
			}
		}
		return 0
	}

	// The glyph for a signal in percent.
	function wifiGlyph(strength) {
		var icons = ["󰤯", "󰤟", "󰤢", "󰤥", "󰤨"]
		var index = Math.max(0, Math.min(4, Math.ceil(strength / 20) - 1))
		return icons[index]
	}
}
