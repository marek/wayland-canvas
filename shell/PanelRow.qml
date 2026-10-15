import QtQuick

// One panel row: an optional leading glyph, a label, an optional smaller
// detail under it, hover, a click. A control the caller parks on the row (a
// switch, a right-edge label) sits above the row's own mouse area, so it
// keeps its clicks; plain text parked there does not take clicks at all.
Item {
	id: root

	property var theme: null
	property string glyph: ""
	property string label: ""
	property string detail: ""
	property bool marked: false // the label in the attention colour
	property bool clickable: true
	signal clicked()

	width: parent ? parent.width : 0
	height: detail !== ""
		? (theme ? theme.rowHeight + theme.detailHeight : 46)
		: (theme ? theme.rowHeight : 32)

	Rectangle {
		anchors.fill: parent
		radius: 4
		visible: mouse.containsMouse && root.enabled
		color: root.theme ? root.theme.hover : Qt.rgba(1, 1, 1, 0.08)
	}

	Row {
		anchors.left: parent.left
		anchors.leftMargin: 8
		anchors.verticalCenter: parent.verticalCenter
		spacing: 8

		Text {
			visible: root.glyph !== ""
			text: root.glyph
			color: root.marked
				? (root.theme ? root.theme.attention : "#f7768e")
				: (root.theme ? root.theme.text : "#a9b1d6")
			font {
				family: root.theme ? root.theme.fontFamily : "monospace"
				pixelSize: root.theme ? root.theme.fontIcon : 13
			}
			renderType: Text.NativeRendering
		}

		Column {
			spacing: 0

			PanelText {
				theme: root.theme
				text: root.label
				color: !root.enabled
					? (root.theme ? root.theme.muted : "#565f89")
					: (root.marked
						? (root.theme ? root.theme.attention : "#f7768e")
						: (root.theme ? root.theme.text : "#a9b1d6"))
			}

			PanelText {
				visible: root.detail !== ""
				theme: root.theme
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
