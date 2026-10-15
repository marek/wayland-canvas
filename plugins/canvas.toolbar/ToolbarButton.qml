import Quickshell
import QtQuick
import WaylandCanvas.Ui

// One toolbar button: its glyph in a square, its label beside it on
// hover — left of it, away from the screen edge — and a click routed to
// the surface, which runs the action, opens the panel through the common
// trigger or unfolds the flyout.
//
// A panel-opening button also names where its panel belongs: the
// toolbar's window spans its whole output, so the button can map itself
// into output coordinates, which the panel card reads (panelRight,
// panelCentreY) to hang the panel to the left of the toolbar, level with
// this button.
Item {
	id: root

	property var spec: null // the registry entry this button shows
	property var surface: null // the toolbar surface, which acts on clicks

	// The label chip's shape: a gap before the square and padding around
	// the text, so the label reads over the canvas.
	readonly property int labelGap: 8
	readonly property int labelPad: 6

	width: surface != null ? surface.buttonSize : 0
	height: width

	TransformWatcher {
		id: watcher
		a: root.surface != null ? root.surface.contentItem : null
		b: root
	}

	// Where a panel opened from this button belongs, in output
	// coordinates: its right edge one margin left of the strip, its
	// centre level with the button. (The strip's settled place: a panel
	// only opens with the strip in place.)
	readonly property real panelRight: {
		watcher.transform
		return surface != null
			? surface.width - 2 * UiTheme.edgeMargin - surface.buttonSize : 0
	}
	readonly property real panelCentreY: {
		watcher.transform
		return mapToItem(null, width / 2, height / 2).y
	}

	// The label, beside the square on hover, on a chip so it reads over
	// whatever the overview is showing.
	Rectangle {
		visible: mouse.containsMouse && root.spec != null && root.spec.label !== ""
		anchors.right: parent.left
		anchors.rightMargin: root.labelGap
		anchors.verticalCenter: parent.verticalCenter
		width: label.implicitWidth + 2 * root.labelPad
		height: label.implicitHeight + 2 * root.labelPad
		radius: 4
		color: UiTheme.background

		PanelText {
			id: label
			anchors.centerIn: parent
			text: root.spec != null ? root.spec.label : ""
		}
	}

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
		onClicked: if (root.surface != null) {
			root.surface.buttonActivated(root)
		}
	}
}
