import QtQuick
import WaylandCanvas.Ui

// One pill of the band or DNS rows: the one in force carries the tint,
// the pinned choice the accent border and weight.
Item {
	id: pill

	property string text: ""
	property bool active: false
	property bool chosen: false
	signal clicked()
	height: 24

	Rectangle {
		anchors.fill: parent
		radius: 4
		border.width: 1
		border.color: pill.chosen ? UiTheme.accent : UiTheme.border
		color: pill.active ? UiTheme.selected : "transparent"
	}

	PanelText {
		anchors.centerIn: parent
		text: pill.text
		font.bold: pill.chosen
	}

	MouseArea {
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: pill.clicked()
	}
}
