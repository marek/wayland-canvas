import QtQuick

// One bar button: a text label, or an icon glyph in a fixed slot. Buttons
// have no hover chrome — the bar paints glyphs only, colour telling state.
Item {
	id: root

	property var theme: null
	property string text: ""
	property bool icon: false // fixed icon slot instead of text padding
	property real textMargin: 8.5 // px of padding either side of a text label
	property bool active: false // paint with the attention colour

	signal pressed(int button)
	signal wheeled(int delta)

	visible: text !== ""
	implicitWidth: icon
		? (theme ? theme.iconSlot : 27)
		: Math.max(12, label.implicitWidth + textMargin * 2)
	implicitHeight: theme ? theme.barHeight : 26
	width: visible ? implicitWidth : 0
	height: visible ? implicitHeight : 0

	Text {
		id: label
		anchors.centerIn: parent
		text: root.text
		color: root.active ? (root.theme ? root.theme.attention : "#f7768e")
			: (root.theme ? root.theme.text : "#a9b1d6")
		font {
			family: root.theme ? root.theme.fontFamily : "monospace"
			pixelSize: root.icon ? (root.theme ? root.theme.fontIcon : 13)
				: (root.theme ? root.theme.fontBody : 12)
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
			root.pressed(mouse.button)
		}
		onWheel: function(wheel) {
			root.wheeled(wheel.angleDelta.y)
		}
	}
}
