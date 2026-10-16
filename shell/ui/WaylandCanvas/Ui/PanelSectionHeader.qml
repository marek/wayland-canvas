import QtQuick
import "."

// The label that opens a panel section ("KNOWN NETWORKS", "TOKENS BY DAY"):
// bold, spaced capitals in the muted colour, between a separator and the
// rows it introduces. Callers set `text` in any case; it is drawn in capitals.
Text {
	textFormat: Text.PlainText
	renderType: Text.NativeRendering
	color: UiTheme.muted
	width: parent ? parent.width : implicitWidth
	height: UiTheme.sectionHeight
	verticalAlignment: Text.AlignVCenter
	elide: Text.ElideRight
	font {
		family: UiTheme.fontFamily
		pixelSize: UiTheme.fontSmall
		bold: true
		capitalization: Font.AllUppercase
		letterSpacing: 1
	}
}
