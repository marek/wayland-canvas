import QtQuick
import "."

// A panel row with an on/off switch on its right. The caller owns the value:
// bind `checked` to the real state and flip that state in `toggled`, which
// fires for a click on the row as for one on the switch.
PanelRow {
	id: root

	property bool checked: false
	signal toggled()

	onClicked: root.toggled()

	PanelSwitch {
		checked: root.checked
		onToggled: root.toggled()
	}
}
