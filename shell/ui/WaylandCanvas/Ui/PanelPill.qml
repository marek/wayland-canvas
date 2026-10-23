import QtQuick
import "."

// One pill of a row of exclusive choices (the Wi-Fi band, the DNS provider):
// the one in force carries the tint, the pinned choice the accent border and
// weight. The row sets the width, each pill an equal share of it.
Item {
	id: root

	property string text: ""
	property bool active: false
	property bool chosen: false
	signal clicked()

	height: UiTheme.controlHeight

	Rectangle {
		anchors.fill: parent
		radius: UiTheme.radius
		border.width: 1
		border.color: root.chosen ? UiTheme.accent : UiTheme.border
		color: root.active ? UiTheme.selected : "transparent"
	}

	PanelText {
		anchors.centerIn: parent
		text: root.text
		font.bold: root.chosen
	}

	MouseArea {
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.clicked()
	}
}
