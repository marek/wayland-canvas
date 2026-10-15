import QtQuick
import "."

// One bar button: a text label, or an icon glyph in a fixed slot. Buttons
// have no hover chrome — the bar paints glyphs only, colour telling state.
// A widget whose manifest names a panel opens it on click without code of
// its own: the bar sets `panel` to the panel's global name and `api` to the
// plugin's, and a left click toggles it.
Item {
	id: root

	property var api: null
	property string panel: "" // the panel a left click opens, "" for none
	property string text: ""
	property bool icon: false // fixed icon slot instead of text padding
	property real textMargin: 8.5 // px of padding either side of a text label
	property bool active: false // paint with the attention colour

	signal pressed(int button)
	signal wheeled(int delta)

	visible: text !== ""
	implicitWidth: icon
		? UiTheme.iconSlot
		: Math.max(12, label.implicitWidth + textMargin * 2)
	implicitHeight: UiTheme.barHeight
	width: visible ? implicitWidth : 0
	height: visible ? implicitHeight : 0

	Text {
		id: label
		anchors.centerIn: parent
		text: root.text
		color: root.active ? UiTheme.attention : UiTheme.text
		font {
			family: UiTheme.fontFamily
			pixelSize: root.icon ? UiTheme.fontIcon : UiTheme.fontBody
		}
		renderType: Text.NativeRendering
		horizontalAlignment: Text.AlignHCenter
		verticalAlignment: Text.AlignVCenter
	}

	MouseArea {
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
		onClicked: function(mouse) {
			if (mouse.button === Qt.LeftButton && root.panel !== "" && root.api != null) {
				root.api.panels.toggle(root.panel, root)
			}
			root.pressed(mouse.button)
		}
		onWheel: function(wheel) {
			root.wheeled(wheel.angleDelta.y)
		}
	}
}
