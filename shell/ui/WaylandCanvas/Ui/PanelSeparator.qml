import QtQuick
import "."

// The hairline between a panel's sections, in the card border's colour.
Rectangle {
	width: parent ? parent.width : implicitWidth
	implicitWidth: 100
	height: 1
	color: UiTheme.border
}
