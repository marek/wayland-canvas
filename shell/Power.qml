import Quickshell.Services.UPower
import QtQuick

// Power: battery level and charging state. Left opens the power menu, right
// toggles the percentage. Hidden without a battery.
BarButton {
	id: root

	property bool showPercentage: false

	readonly property var battery: UPower.displayDevice
	readonly property bool present: battery != null && battery.isPresent
	readonly property bool charging: present
		&& (battery.state === UPowerDeviceState.Charging
			|| battery.state === UPowerDeviceState.PendingCharge)

	text: present
		? (showPercentage ? Math.round(battery.percentage) + "% " + glyph() : glyph())
		: ""
	visible: present

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			showPercentage = !showPercentage
		} else {
			menu.visible = !menu.visible
		}
	}

	PowerMenu {
		id: menu
		theme: root.theme
		anchorItem: root
	}

	function glyph() {
		var discharging = ["󰁺", "󰁻", "󰁼", "󰁽", "󰁾", "󰁿", "󰂀", "󰂁", "󰂂", "󰁹"]
		var chargingIcons = ["󰢜", "󰂆", "󰂇", "󰂈", "󰢝", "󰂉", "󰢞", "󰂊", "󰂋", "󰂅"]
		var index = Math.max(0, Math.min(9, Math.floor(battery.percentage / 10)))
		if (battery.state === UPowerDeviceState.FullyCharged) {
			return "󰂅"
		}
		return charging ? chargingIcons[index] : discharging[index]
	}
}
