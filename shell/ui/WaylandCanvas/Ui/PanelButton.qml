import QtQuick
import "."

// A button in four kinds: "fill" (the default), a label on a filled box;
// "primary", the same on the accent; "outline", a hairline box that fills under
// the pointer; "flat", the label alone until the pointer is over it, with a hit
// area wider than the text. `icon` sets the label as a glyph in a fixed square.
Item {
	id: root

	property string kind: "fill" // fill, primary, outline or flat
	property string text: ""
	property bool icon: false
	property bool dim: false // flat: the muted register at rest
	signal clicked()

	readonly property bool flat: kind === "flat"

	implicitWidth: flat ? label.implicitWidth
		: icon ? UiTheme.buttonSize
		: label.implicitWidth + 2 * UiTheme.panelPadding
	implicitHeight: flat ? label.implicitHeight
		: icon ? UiTheme.buttonSize : UiTheme.controlHeight

	Rectangle {
		anchors.fill: parent
		visible: !root.flat
		radius: UiTheme.radius
		border.width: root.kind === "outline" ? 1 : 0
		border.color: UiTheme.border
		color: {
			if (root.kind === "primary") {
				return UiTheme.accent
			}
			if (root.kind === "outline") {
				return mouse.containsMouse ? UiTheme.hover : "transparent"
			}
			return mouse.containsMouse ? UiTheme.hover : UiTheme.fill
		}
	}

	PanelText {
		id: label
		anchors.centerIn: parent
		text: root.text
		color: {
			if (root.kind === "primary") {
				return UiTheme.onAccent
			}
			if (root.flat && mouse.containsMouse) {
				return UiTheme.attention
			}
			return root.flat && root.dim ? UiTheme.muted : UiTheme.text
		}
		font.pixelSize: root.icon ? UiTheme.fontIcon
			: root.flat ? UiTheme.fontBody : UiTheme.fontSmall
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		anchors.margins: root.flat ? -6 : 0
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.clicked()
	}
}
