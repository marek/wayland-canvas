import QtQuick
import "."

// One choice in a group of exclusive ones: the label, a one-line
// description under it, and a dot on the right that marks the chosen
// row. The caller owns the value and lights one row's `chosen`; a click
// is a `picked`, whoever owns the group turns it into the set.
Item {
	id: root

	property string label: ""
	property string description: ""
	property string note: "" // an error line, drawn in the attention colour
	property bool chosen: false
	signal picked()

	width: parent ? parent.width : 0
	height: UiTheme.rowHeight
		+ (root.description !== "" || root.note !== "" ? UiTheme.detailHeight : 0)

	Rectangle {
		anchors.fill: parent
		radius: 4
		visible: root.chosen
		color: UiTheme.selected
	}

	Rectangle {
		anchors.fill: parent
		radius: 4
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	Column {
		anchors.left: parent.left
		anchors.leftMargin: 8
		anchors.verticalCenter: parent.verticalCenter
		spacing: 0

		PanelText {
			text: root.label
			color: root.chosen ? UiTheme.accent : UiTheme.text
		}

		PanelText {
			visible: root.note !== ""
			text: root.note
			color: UiTheme.attention
		}

		PanelText {
			visible: root.note === "" && root.description !== ""
			dim: true
			text: root.description
		}
	}

	Rectangle {
		anchors.right: parent.right
		anchors.rightMargin: 8
		anchors.verticalCenter: parent.verticalCenter
		width: 8
		height: 8
		radius: 4
		visible: root.chosen
		color: UiTheme.accent
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.picked()
	}
}
