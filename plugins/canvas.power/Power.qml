import QtQuick
import WaylandCanvas.Ui
import "."

// Power: battery level and charging state. Left opens the power panel (the
// manifest names it); right still toggles the percentage. Hidden without a
// battery.
BarButton {
	id: root

	property bool showPercentage: false

	text: PowerState.present
		? (showPercentage ? PowerState.level + "% " + PowerState.glyph() : PowerState.glyph())
		: ""
	visible: PowerState.present

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			showPercentage = !showPercentage
		}
	}
}
