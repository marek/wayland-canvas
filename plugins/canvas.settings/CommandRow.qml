import QtQuick
import WaylandCanvas.Ui

// One command row in the Shortcuts tab: its chord as a chip (a click on
// the chip records a new one), the command in a field (Enter saves) and
// a Remove button. A fault — a recording or a save that failed — shows
// its line under the row.
Item {
	id: root

	property var command: null // {keys, command}; a draft holds keys ""
	property bool draft: false
	property bool recording: false
	property string fault: ""

	signal record(string text) // the field's text as typed, saved or not
	signal save(string text)
	signal remove()

	width: parent ? parent.width : 0
	height: UiTheme.rowHeight + (root.detail !== "" ? UiTheme.detailHeight : 0)

	readonly property string detail: root.fault !== ""
		? root.fault
		: root.recording ? "Press the new keys — Esc cancels" : ""

	Rectangle {
		anchors.fill: parent
		radius: UiTheme.radius
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	// The whole row records too, except where the field takes the click.
	MouseArea {
		id: mouse
		anchors.left: parent.left
		anchors.right: field.left
		height: parent.height
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.record(field.text)
	}

	KeyChip {
		id: chip
		anchors.left: parent.left
		anchors.leftMargin: UiTheme.rowInset
		anchors.verticalCenter: parent.verticalCenter
		chord: root.command && root.command.keys ? root.command.keys : "None"
		onClicked: root.record(field.text)
	}

	PanelField {
		id: field
		anchors.left: chip.right
		anchors.leftMargin: UiTheme.rowInset
		anchors.right: removeButton.left
		anchors.rightMargin: UiTheme.rowInset
		anchors.verticalCenter: parent.verticalCenter
		height: UiTheme.controlHeight
		text: root.command ? root.command.command : ""
		onAccepted: root.save(text)
	}

	PanelButton {
		id: removeButton
		kind: "flat"
		dim: true
		anchors.right: parent.right
		anchors.rightMargin: UiTheme.rowInset
		anchors.verticalCenter: parent.verticalCenter
		text: "Remove"
		onClicked: root.remove()
	}

	PanelText {
		visible: root.detail !== ""
		x: UiTheme.rowInset
		y: UiTheme.rowHeight
		width: parent.width - 2 * UiTheme.rowInset
		elide: Text.ElideRight
		color: UiTheme.attention
		text: root.detail
	}
}
