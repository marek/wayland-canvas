import Quickshell.Networking
import QtQuick
import WaylandCanvas.Ui
import "."

// Network: connection type and Wi-Fi strength. A left click opens the panel
// (the manifest names it); NetworkState holds what the glyph and the panel show.
BarButton {
	id: root

	icon: true
	visible: NetworkState.device != null
	text: {
		var device = NetworkState.device
		if (device == null || !device.connected) {
			return "󰤮"
		}
		if (NetworkState.wifi != null) {
			return NetworkState.wifiGlyph(NetworkState.strength)
		}
		return device.type === DeviceType.Ethernet ? "󰈀" : "󰤮"
	}
}
