import QtQuick
import "."

// One panel row: an optional leading glyph, a label, an optional smaller
// detail under it, selection, hover, a click. A control the caller parks on
// the row (a switch, a right-edge label) sits above the row's own mouse
// area, so it keeps its clicks; plain text parked there does not take
// clicks at all.
Item {
	id: root

	property string glyph: ""
	property string label: ""
	property string detail: ""
	property bool marked: false // the label in the attention colour
	property bool selected: false // the accent-tinted fill of the chosen row
	property bool clickable: true
	signal clicked()

	width: parent ? parent.width : 0
	height: detail !== ""
		? UiTheme.rowHeight + UiTheme.detailHeight
		: UiTheme.rowHeight

	// The chosen row's tint, with hover drawn over it.
	Rectangle {
		anchors.fill: parent
		radius: 4
		visible: root.selected && root.enabled
		color: UiTheme.selected
	}

	Rectangle {
		anchors.fill: parent
		radius: 4
		visible: mouse.containsMouse && root.enabled
		color: UiTheme.hover
	}

	Row {
		anchors.left: parent.left
		anchors.leftMargin: 8
		anchors.verticalCenter: parent.verticalCenter
		spacing: 8

		Text {
			visible: root.glyph !== ""
			text: root.glyph
			color: root.marked ? UiTheme.attention : UiTheme.text
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontIcon
			}
			renderType: Text.NativeRendering
		}

		Column {
			spacing: 0

			PanelText {
				text: root.label
				color: !root.enabled
					? UiTheme.muted
					: (root.marked ? UiTheme.attention : UiTheme.text)
			}

			PanelText {
				visible: root.detail !== ""
				dim: true
				text: root.detail
			}
		}
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		enabled: root.enabled && root.clickable
		hoverEnabled: root.enabled && root.clickable
		cursorShape: root.clickable && root.enabled
			? Qt.PointingHandCursor : Qt.ArrowCursor
		onClicked: root.clicked()
	}
}
