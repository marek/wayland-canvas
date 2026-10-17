import QtQuick
import WaylandCanvas.Ui

// One toolbar button: its glyph in a square, a click routed to the
// surface, which runs the action, opens the panel through the common
// trigger or unfolds the flyout. The button places itself in the strip
// (x and y bound to the surface's layout of its buttons), so a live
// edge change moves it with the strip, and reports its hover, so the
// surface can show its label beside it on the chrome surface.
//
// A panel-opening button also names where its panel belongs, in output
// coordinates: on the toolbar's inner side, one margin past the settled
// strip, level with this button along it. Which properties carry that
// depends on the edge — a right-edge strip gives panelRight with
// panelCentreY (the card hangs to the left of it), a left one panelLeft
// with panelCentreY, a top or bottom row panelTop or panelBottom with
// panelCentreX — and a card reads whichever pair is defined
// (shell/ui/WaylandCanvas/Ui/PanelCard.qml).
Item {
	id: root

	property var spec: null // the registry entry this button shows
	property var surface: null // the toolbar surface, which acts on clicks
	property int index: -1 // its place in the strip

	width: surface != null ? surface.buttonSize : 0
	height: width
	x: surface != null && surface.vertical ? 0 : index * surface.buttonSize
	y: surface != null && surface.vertical ? index * surface.buttonSize : 0

	// This button's square, in output coordinates, from the surface's
	// layout of the strip. (The settled place: a panel only opens with
	// the strip in place.)
	readonly property var spot: surface != null && index >= 0
		? surface.buttonBox(index) : null

	// Where a panel opened from this button belongs — undefined on every
	// property the edge does not use, so a bar widget (which names none
	// of them) and a button on another edge are told apart.
	readonly property var panelRight: spot != null && surface.edge === "right"
		? spot.x - surface.edgeMargin : undefined
	readonly property var panelLeft: spot != null && surface.edge === "left"
		? spot.x + spot.width + surface.edgeMargin : undefined
	readonly property var panelTop: spot != null && surface.edge === "top"
		? spot.y + spot.height + surface.edgeMargin : undefined
	readonly property var panelBottom: spot != null && surface.edge === "bottom"
		? spot.y - surface.edgeMargin : undefined
	readonly property var panelCentreY: spot != null && surface.vertical
		? spot.y + spot.height / 2 : undefined
	readonly property var panelCentreX: spot != null && !surface.vertical
		? spot.x + spot.width / 2 : undefined

	// The square: a hover wash and the glyph.
	Rectangle {
		anchors.fill: parent
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	Text {
		anchors.centerIn: parent
		text: root.spec != null ? root.spec.icon : ""
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
