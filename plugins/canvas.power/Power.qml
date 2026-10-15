import Quickshell.Services.UPower
import QtQuick
import WaylandCanvas.Ui

// Power: battery level and charging state. Left opens the power panel (the
// manifest names it); right still toggles the percentage. Hidden without a
// battery.
BarButton {
	id: root

	property bool showPercentage: false

	readonly property var battery: UPower.displayDevice
	readonly property bool present: battery != null && battery.isPresent
	// The charge in percent; UPower's percentage arrives as 0 to 1.
	readonly property int level: present ? Math.round(battery.percentage * 100) : 0
	readonly property bool charging: present
		&& (battery.state === UPowerDeviceState.Charging
			|| battery.state === UPowerDeviceState.PendingCharge)

	text: present
		? (showPercentage ? level + "% " + glyph() : glyph())
		: ""
	visible: present

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			showPercentage = !showPercentage
		}
	}

	function glyph() {
		var discharging = ["󰁺", "󰁻", "󰁼", "󰁽", "󰁾", "󰁿", "󰂀", "󰂁", "󰂂", "󰁹"]
		var chargingIcons = ["󰢜", "󰂆", "󰂇", "󰂈", "󰢝", "󰂉", "󰢞", "󰂊", "󰂋", "󰂅"]
		var index = Math.max(0, Math.min(9, Math.floor(level / 10)))
		if (battery.state === UPowerDeviceState.FullyCharged) {
			return "󰂅"
		}
		return charging ? chargingIcons[index] : discharging[index]
	}
}
