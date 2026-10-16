import Quickshell.Networking
import QtQuick
import WaylandCanvas.Ui

// Network: connection type and Wi-Fi strength. A left click opens the panel
// (the manifest names it): the connection in use with its live figures, the
// band, the DNS provider, and the networks to join.
BarButton {
	id: root

	readonly property var device: currentDevice()
	readonly property var wifi: device != null && device.type === DeviceType.Wifi
		? device
		: null
	readonly property int strength: connectedStrength()
	// The network the connection in use is on, null when none is; the
	// panel's head and figures describe this one.
	readonly property var connectedNetwork: connectedNet()

	// The visible networks, strongest first — the bar glyph's strength read.
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

	function connectedNet() {
		if (wifi == null || wifi.networks == null) {
			return null
		}
		var networks = wifi.networks.values
		for (var i = 0; i < networks.length; i++) {
			if (networks[i].connected) {
				return networks[i]
			}
		}
		return null
	}

	function connectedStrength() {
		return connectedNet() != null ? percent(connectedNet()) : 0
	}

	// The glyph for a signal in percent.
	function wifiGlyph(strength) {
		var icons = ["󰤯", "󰤟", "󰤢", "󰤥", "󰤨"]
		var index = Math.max(0, Math.min(4, Math.ceil(strength / 20) - 1))
		return icons[index]
	}
}