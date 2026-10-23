import QtQuick
import "."

// The head of a panel: a large glyph (or the caller's `iconComponent`), the
// title in bold and a meta line in muted spaced capitals, in the attention
// colour when it names a fault. The children are the header controls, laid in
// a row on the right.
Item {
	id: root

	property string glyph: ""
	property Component iconComponent: null
	property string title: ""
	property string meta: ""
	property bool metaAttention: false
	default property alias controls: controlRow.data

	width: parent ? parent.width : implicitWidth
	height: UiTheme.heroHeight

	Loader {
		id: icon
		width: UiTheme.heroIcon
		height: UiTheme.heroIcon
		anchors.left: parent.left
		anchors.verticalCenter: parent.verticalCenter
		sourceComponent: root.iconComponent !== null ? root.iconComponent : glyphIcon
	}

	Component {
		id: glyphIcon

		Text {
			text: root.glyph
			textFormat: Text.PlainText
			renderType: Text.NativeRendering
			horizontalAlignment: Text.AlignHCenter
			verticalAlignment: Text.AlignVCenter
			color: UiTheme.text
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.heroIcon - 4
			}
		}
	}

	Column {
		anchors.left: icon.right
		anchors.leftMargin: UiTheme.panelPadding
		anchors.right: controlRow.left
		anchors.rightMargin: controlRow.width > 0 ? UiTheme.panelSpacing : 0
		anchors.verticalCenter: parent.verticalCenter
		spacing: 2

		Text {
			width: parent.width
			visible: root.title !== ""
			text: root.title
			textFormat: Text.PlainText
			renderType: Text.NativeRendering
			elide: Text.ElideRight
			color: UiTheme.text
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontTitle
				bold: true
			}
		}

		Text {
			width: parent.width
			visible: root.meta !== ""
			text: root.meta
			textFormat: Text.PlainText
			renderType: Text.NativeRendering
			elide: Text.ElideRight
			color: root.metaAttention ? UiTheme.attention : UiTheme.muted
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontSmall
				bold: true
				capitalization: Font.AllUppercase
				letterSpacing: 1
			}
		}
	}

	Row {
		id: controlRow
		anchors.right: parent.right
		anchors.verticalCenter: parent.verticalCenter
		spacing: UiTheme.panelSpacing
	}
}
