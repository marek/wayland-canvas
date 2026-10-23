import QtQuick
import "."

// One panel row: an optional leading glyph or colour swatch, a label, an
// optional detail under it, selection, hover, a click. What the caller
// declares inside goes into the trailing slot, laid out right to left above
// the row's mouse area so it keeps its clicks; plain text there takes none.
Item {
	id: root

	property string glyph: ""
	property string swatch: "" // a colour, in place of the glyph
	property string label: ""
	property string detail: ""
	property bool marked: false // the label in the attention colour
	property bool selected: false // the accent-tinted fill of the chosen row
	property bool clickable: true
	default property alias trailing: trailingRow.data
	signal clicked()

	width: parent ? parent.width : 0
	height: detail !== ""
		? UiTheme.rowHeight + UiTheme.detailHeight
		: UiTheme.rowHeight

	// The chosen row's tint, with hover drawn over it.
	Rectangle {
		anchors.fill: parent
		radius: UiTheme.radius
		visible: root.selected && root.enabled
		color: UiTheme.selected
	}

	Rectangle {
		anchors.fill: parent
		radius: UiTheme.radius
		visible: mouse.containsMouse && root.enabled
		color: UiTheme.hover
	}

	Row {
		id: line
		anchors.left: parent.left
		anchors.leftMargin: UiTheme.rowInset
		anchors.right: trailingRow.left
		anchors.rightMargin: trailingRow.width > 0 ? UiTheme.panelSpacing : 0
		anchors.verticalCenter: parent.verticalCenter
		spacing: UiTheme.rowInset

		Text {
			id: glyphText
			visible: root.glyph !== ""
			text: root.glyph
			color: root.marked ? UiTheme.attention : UiTheme.text
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontIcon
			}
			renderType: Text.NativeRendering
		}

		Rectangle {
			id: swatchBox
			visible: root.swatch !== ""
			width: UiTheme.iconSize
			height: UiTheme.iconSize
			radius: 3
			color: root.swatch !== "" ? root.swatch : "transparent"
		}

		// Bounded by what the glyph or swatch and the trailing slot leave:
		// both lines are elided inside it, never drawn past the row.
		Column {
			width: line.width
				- (glyphText.visible ? glyphText.width + line.spacing : 0)
				- (swatchBox.visible ? swatchBox.width + line.spacing : 0)
			spacing: 0

			PanelText {
				width: parent.width
				elide: Text.ElideRight
				text: root.label
				color: !root.enabled
					? UiTheme.muted
					: (root.marked ? UiTheme.attention : UiTheme.text)
			}

			PanelText {
				width: parent.width
				elide: Text.ElideRight
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

	Row {
		id: trailingRow
		anchors.right: parent.right
		anchors.rightMargin: UiTheme.rowInset
		anchors.verticalCenter: parent.verticalCenter
		spacing: UiTheme.panelSpacing
	}
}
