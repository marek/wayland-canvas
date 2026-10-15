import QtQuick
import "."

// Panel text in the shell's own font and palette; `dim` is the quieter
// register of section labels, placeholders and off-state notes.
Text {
	property bool dim: false

	textFormat: Text.PlainText
	renderType: Text.NativeRendering
	color: dim ? UiTheme.muted : UiTheme.text
	font {
		family: UiTheme.fontFamily
		pixelSize: UiTheme.fontBody
	}
}
