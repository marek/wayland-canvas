import QtQuick
import WaylandCanvas.Ui

// One flyout row: a glyph or a colour swatch, a label, hover, a click.
// A panel row's shape (PanelRow), but a swatch is not a glyph, so the
// flyout's row is its own piece.
Item {
	id: root

	property var entry: null

	signal chosen()

	width: parent ? parent.width : 0
	height: UiTheme.rowHeight

	Rectangle {
		anchors.fill: parent
		radius: 4
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	Row {
		anchors.left: parent.left
		anchors.leftMargin: 8
		anchors.verticalCenter: parent.verticalCenter
		spacing: 8

		// A glyph, or a colour swatch when the entry names a colour.
		Text {
			visible: root.entry != null && root.entry.icon !== undefined
				&& root.entry.icon !== ""
			text: root.entry != null && root.entry.icon !== undefined
				? root.entry.icon : ""
			color: UiTheme.text
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontIcon
			}
			renderType: Text.NativeRendering
		}

		Rectangle {
			visible: root.entry != null && root.entry.color !== undefined
				&& root.entry.color !== ""
			width: UiTheme.iconSize
			height: UiTheme.iconSize
			radius: 3
			color: root.entry != null && root.entry.color !== undefined
				? root.entry.color : "transparent"
		}

		PanelText {
			text: root.entry != null && root.entry.label !== undefined
				? root.entry.label : ""
		}
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.chosen()
	}
}
