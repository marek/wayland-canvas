pragma Singleton

import Quickshell.Networking
import QtQuick

// What the network widget on every bar and the network panel share: the
// device the connection is on, its Wi-Fi side, and the network in use.
QtObject {
	// The device to describe: a connected one, else the first (the backend
	// only lists devices the system actually has).
	readonly property var device: currentDevice()
	readonly property var wifi: device != null && device.type === DeviceType.Wifi
		? device
		: null
	// The network the connection in use is on, null when none is; the
	// panel's head and figures describe this one.
	readonly property var connectedNetwork: connectedNet()
	readonly property int strength: connectedNetwork != null ? percent(connectedNetwork) : 0

	function currentDevice() {
		var devices = Networking.devices ? Networking.devices.values : []
		for (var i = 0; i < devices.length; i++) {
			if (devices[i].connected) {
				return devices[i]
			}
		}
		return devices.length > 0 ? devices[0] : null
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

	// A network's signal in percent; the backend reports 0 to 1.
	function percent(network) {
		return Math.round(network.signalStrength * 100)
	}

	// The glyph for a signal in percent.
	function wifiGlyph(strength) {
		var icons = ["󰤯", "󰤟", "󰤢", "󰤥", "󰤨"]
		var index = Math.max(0, Math.min(4, Math.ceil(strength / 20) - 1))
		return icons[index]
	}
}
