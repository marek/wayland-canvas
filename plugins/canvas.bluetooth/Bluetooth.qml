import QtQuick
import WaylandCanvas.Ui
import "."

// Bluetooth: radio state, connected mark while a device holds a link. A
// left click opens the panel (the manifest names it); right-click still
// flips the radio. Hidden without an adapter.
BarButton {
	id: root

	icon: true
	visible: BluetoothState.adapter != null
	text: !BluetoothState.radioOn
		? "󰂲"
		: (BluetoothState.linked ? "󰂱" : "󰂯")

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			BluetoothState.adapter.enabled = !BluetoothState.adapter.enabled
		}
	}
}
