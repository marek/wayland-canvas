import QtQuick

// Panel text in the bar's own font and palette; `dim` is the quieter register
// of section labels, placeholders and off-state notes.
Text {
	property var theme: null
	property bool dim: false

	textFormat: Text.PlainText
	renderType: Text.NativeRendering
	color: dim
		? (theme ? theme.muted : "#565f89")
		: (theme ? theme.text : "#a9b1d6")
	font {
		family: theme ? theme.fontFamily : "monospace"
		pixelSize: theme ? theme.fontBody : 12
	}
}
