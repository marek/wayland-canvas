import QtQuick
import QtQuick.Controls
import "."

// A single-line field with the panel's own look; `password` masks the text.
// Enter is the caller's to interpret (the inherited `accepted` signal).
TextField {
	id: root

	property bool password: false

	echoMode: password ? TextInput.Password : TextInput.Normal
	color: UiTheme.text
	selectionColor: UiTheme.accent
	selectedTextColor: UiTheme.onAccent
	placeholderTextColor: UiTheme.muted
	font {
		family: UiTheme.fontFamily
		pixelSize: UiTheme.fontBody
	}
	background: Rectangle {
		radius: 4
		color: UiTheme.fill
		border.width: 1
		border.color: root.activeFocus ? UiTheme.accent : "transparent"
	}
}
