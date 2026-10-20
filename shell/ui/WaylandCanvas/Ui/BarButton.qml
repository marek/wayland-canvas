import QtQuick
import "."

// One bar button: a text label, or an icon glyph in a fixed slot. Buttons
// have no hover chrome — the bar paints glyphs only, colour telling state.
// A widget whose manifest names a panel opens it on click without code of
// its own: the bar sets `panel` to the panel's global name and `api` to the
// plugin's, and a left click toggles it. A wheel turn reaches the widget as
// whole notches (`wheeled`), the small deltas of a smooth wheel adding up.
Item {
	id: root

	property var api: null
	property string panel: "" // the panel a left click opens, "" for none
	property string text: ""
	property bool icon: false // fixed icon slot instead of text padding
	property real textMargin: 8.5 // px of padding either side of a text label
	property bool active: false // paint with the attention colour

	signal pressed(int button)
	signal wheeled(int notches) // positive up, one per notchUnits of turn

	readonly property int notchUnits: 120 // angleDelta of one wheel notch
	property real turned: 0 // the turn not yet a whole notch

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
			root.turn(wheel.angleDelta.y)
		}
	}

	// A direction change drops the half-done turn of the other direction.
	function turn(delta) {
		if (delta * turned < 0) {
			turned = 0
		}
		turned += delta
		var notches = Math.trunc(turned / notchUnits)
		turned -= notches * notchUnits
		if (notches !== 0) {
			wheeled(notches)
		}
	}
}
