import QtQuick
import WaylandCanvas.Ui

// One toolbar button: its glyph in a square. A click goes to the surface,
// which runs the action, toggles the panel or unfolds the flyout; the hover is
// reported so the strip can show the label. A panel-opening button names where
// its panel belongs in `panelSpot` (the edge and the button's box), which the
// PanelCard places itself from.
Item {
	id: root

	required property var modelData // the manifest entry this button shows
	required property int index // its place in the strip
	property var surface: null // the toolbar surface, which acts on clicks

	readonly property var spec: modelData

	width: surface != null ? surface.squareSize : 0
	height: width

	// Where a panel opened from this button belongs (the settled place: a
	// panel only opens with the strip in place).
	readonly property var panelSpot: surface != null
		? {edge: surface.edge, box: surface.squareBox(index)} : undefined

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
