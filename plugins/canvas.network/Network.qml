import Quickshell.Networking
import QtQuick
import WaylandCanvas.Ui
import "."

// Network: connection type and Wi-Fi strength. A left click opens the panel
// (the manifest names it): the connection in use with its live figures, the
// band, the DNS provider, and the networks to join. What the glyph shows
// is NetworkState's, the panel's too.
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
