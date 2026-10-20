import QtQuick
import WaylandCanvas.Ui

// One toolbar button: its glyph in a square, a click routed to the
// surface, which runs the action, opens the panel through the common
// trigger or unfolds the flyout. The strip lays the button out, so a live
// edge change moves it with the strip; the button reports its hover, so the
// strip can show its label beside it.
//
// A panel-opening button also names where its panel belongs: beside the
// strip, level with this button, in output coordinates (`panelSpot`: the
// edge and the button's box). A card places itself from it
// (shell/ui/WaylandCanvas/Ui/PanelCard.qml); a bar widget has none.
Item {
	id: root

	required property var modelData // the registry entry this button shows
	required property int index // its place in the strip
	property var surface: null // the toolbar surface, which acts on clicks

	readonly property var spec: modelData

	width: surface != null ? surface.squareSize : 0
	height: width

	// Where a panel opened from this button belongs (the settled place: a
	// panel only opens with the strip in place).
	readonly property var panelSpot: surface != null
		? {edge: surface.edge, box: surface.squareBox(index)} : undefined

	// The square: a hover wash and the glyph.
	Rectangle {
		anchors.fill: parent
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	Text {
		anchors.centerIn: parent
		text: root.spec.icon
		color: UiTheme.text
		font {
			family: UiTheme.fontFamily
			pixelSize: UiTheme.fontIcon
		}
		renderType: Text.NativeRendering
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onContainsMouseChanged: {
			if (root.surface == null) {
				return
			}
			if (mouse.containsMouse) {
				root.surface.hoverButton = root
			} else if (root.surface.hoverButton === root) {
				root.surface.hoverButton = null
			}
		}
		onClicked: if (root.surface != null) {
			root.surface.buttonActivated(root)
		}
	}
}
