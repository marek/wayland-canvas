import QtQuick
import "."

// One keyboard chord as a chip: the chord's text on a filled rounded
// box that reads as a key cap. Clickable where the caller records a new
// chord by clicking it (a command's row); a plain display otherwise.
Item {
	id: root

	property string chord: ""
	signal clicked()

	readonly property int chipPad: 6

	implicitWidth: Math.max(20, label.implicitWidth + 2 * chipPad)
	implicitHeight: 20

	Rectangle {
		anchors.fill: parent
		radius: UiTheme.radius
		color: UiTheme.fill
		border.width: 1
		border.color: mouse.containsMouse ? UiTheme.accent : "transparent"
	}

	PanelText {
		id: label
		anchors.centerIn: parent
		text: root.chord
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.clicked()
	}
}
