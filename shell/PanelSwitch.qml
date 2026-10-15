import QtQuick

// An on/off switch: a track with a sliding knob. The caller owns the value —
// bind `checked` to the real state and flip that state in `toggled`.
Item {
	id: root

	property var theme: null
	property bool checked: false
	signal toggled()

	implicitWidth: 38
	implicitHeight: 20

	Rectangle {
		id: track
		anchors.centerIn: parent
		width: 34
		height: 16
		radius: 8
		color: root.checked
			? (root.theme ? root.theme.accent : "#7aa2f7")
			: Qt.rgba(1, 1, 1, 0.14)

		Behavior on color { ColorAnimation { duration: 100 } }

		Rectangle {
			width: 12
			height: 12
			radius: 6
			anchors.verticalCenter: parent.verticalCenter
			x: root.checked ? parent.width - width - 2 : 2
			color: root.theme ? root.theme.background : "#1a1b26"

			Behavior on x {
				NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
			}
		}
	}

	MouseArea {
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.toggled()
	}
}
