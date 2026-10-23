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
		radius: UiTheme.radius
		visible: root.chosen
		color: UiTheme.selected
	}

	Rectangle {
		anchors.fill: parent
		radius: UiTheme.radius
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	// The text stops short of the dot's place on the right, so a long
	// line is cut with an ellipsis inside the row, never drawn past the
	// card's edge.
	Column {
		anchors.left: parent.left
		anchors.leftMargin: UiTheme.rowInset
		anchors.verticalCenter: parent.verticalCenter
		width: parent.width - UiTheme.rowInset - 24
		spacing: 0

		PanelText {
			width: parent.width
			elide: Text.ElideRight
			text: root.label
			color: root.chosen ? UiTheme.accent : UiTheme.text
		}

		PanelText {
			width: parent.width
			elide: Text.ElideRight
			visible: root.note !== ""
			text: root.note
			color: UiTheme.attention
		}

		PanelText {
			width: parent.width
			elide: Text.ElideRight
			visible: root.note === "" && root.description !== ""
			dim: true
			text: root.description
		}
	}

	Rectangle {
		anchors.right: parent.right
		anchors.rightMargin: UiTheme.rowInset
		anchors.verticalCenter: parent.verticalCenter
		width: 8
		height: 8
		radius: width / 2
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
