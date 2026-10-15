import QtQuick
import QtQuick.Controls

// A single-line field with the panel's own look; `password` masks the text.
// Enter is the caller's to interpret (the inherited `accepted` signal).
TextField {
	id: root

	property var theme: null
	property bool password: false

	echoMode: password ? TextInput.Password : TextInput.Normal
	color: theme ? theme.text : "#a9b1d6"
	selectionColor: theme ? theme.accent : "#7aa2f7"
	selectedTextColor: color
	placeholderTextColor: theme ? theme.muted : "#565f89"
	font {
		family: theme ? theme.fontFamily : "monospace"
		pixelSize: theme ? theme.fontBody : 12
	}
	background: Rectangle {
		radius: 4
		color: Qt.rgba(1, 1, 1, 0.06)
		border.width: 1
		border.color: root.activeFocus
			? (root.theme ? root.theme.accent : "#7aa2f7")
			: "transparent"
	}
}
