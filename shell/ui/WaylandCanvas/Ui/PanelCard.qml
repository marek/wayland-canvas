import Quickshell
import Quickshell.Wayland
import QtQuick
import "."

// One panel: a full-output overlay layer surface holding a card under its
// anchor. A click outside the card or Escape closes it and `dismissed` tells
// the PanelsManager. A layer surface, not an xdg popup: the Qt platform
// refuses a popup on a layer-shell parent.
PanelWindow {
	id: root

	property var api: null
	property var anchorItem: null // the bar widget or toolbar button it hangs from
	// The panel's manifest id; it names the layer surface
	// `wayland-canvas-panel-<panelName>`.
	property string panelName: ""
	property int panelWidth: 240

	default property alias content: column.data
	property alias spacing: column.spacing
	property bool open: false

	// The panel closed itself (a click outside, or Escape); the panel
	// manager forgets it.
	signal dismissed()

	visible: open
	color: "transparent"
	exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation

	readonly property var anchorWindow: anchorItem ? anchorItem.QsWindow.window : null
	screen: anchorWindow ? anchorWindow.screen : null

	WlrLayershell.namespace: "wayland-canvas-panel-" + panelName
	WlrLayershell.layer: WlrLayer.Overlay
	// On demand, so a text field takes typing without a click first and Escape
	// is caught here before any field sees it.
	WlrLayershell.keyboardFocus: open ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

	anchors {
		top: true
		bottom: true
		left: true
		right: true
	}

	// Everything but the bar strip is this panel's to catch, so a second
	// click of an indicator reaches the bar and toggles the panel closed. A
	// toolbar button's strip lies below the panel layer, so there the card
	// catches the whole output.
	readonly property real maskTop: beside ? 0 : barH
	mask: Region {
		y: root.maskTop
		width: root.screenW
		height: root.screenH - root.maskTop
	}

	function showPanel() {
		open = true
	}

	function close() {
		if (open) {
			open = false
			dismissed()
		}
	}

	// The compositor hides the surface behind `open`'s back if it is ever
	// forced off the screen: resync, and tell the manager this panel went.
	onVisibleChanged: if (!visible && open) {
		open = false
		dismissed()
	}

	readonly property real screenW: anchorWindow && anchorWindow.screen
		? anchorWindow.screen.width : UiTheme.screenWidth
	readonly property real screenH: anchorWindow && anchorWindow.screen
		? anchorWindow.screen.height : UiTheme.screenHeight
	readonly property real barH: anchorWindow ? anchorWindow.height : UiTheme.barHeight

	// Re-runs cardPos when the bar lays its widgets out or moves them: a bare
	// mapToItem binding would only see the first, pre-layout position.
	TransformWatcher {
		id: anchorWatcher
		a: root.anchorWindow ? root.anchorWindow.contentItem : null
		b: root.anchorItem
	}

	// True when the anchor names its own spot (the toolbar's buttons); a
	// bar widget names none.
	readonly property bool beside: anchorItem != null
		&& anchorItem.panelSpot !== undefined

	// The card hangs centred under a bar widget, kept inside the output; an
	// anchor with a `panelSpot` (a toolbar button, whose window is only the
	// strip) has it beside the strip, level with the button.
	readonly property point cardPos: {
		anchorWatcher.transform
		if (beside) {
			var spot = anchorItem.panelSpot
			return StripLayout.placeBeside(spot.edge, spot.box, panelWidth,
				card.height, Qt.size(screenW, screenH), UiTheme.edgeMargin, true)
		}
		var centre = anchorItem ? anchorItem.mapToItem(null, anchorItem.width / 2, 0).x : 0
		return Qt.point(StripLayout.clampInside(centre - panelWidth / 2, panelWidth, screenW),
			barH)
	}

	Item {
		anchors.fill: parent

		// The dismissal overlay: a click anywhere the card is not.
		MouseArea {
			anchors.fill: parent
			onClicked: root.close()
		}

		Rectangle {
			id: card

			x: root.cardPos.x
			y: root.cardPos.y
			width: root.panelWidth
			height: column.implicitHeight + 2 * UiTheme.panelPadding
			color: UiTheme.panel
			border.width: 1
			border.color: UiTheme.border

			// Swallows clicks on the card's own padding so only clicks
			// truly outside close the panel.
			MouseArea {
				anchors.fill: parent
			}

			Column {
				id: column
				focus: true
				Keys.priority: Keys.BeforeItem
				Keys.onEscapePressed: root.close()
				x: UiTheme.panelPadding
				y: UiTheme.panelPadding
				width: parent.width - 2 * x
				spacing: UiTheme.panelSpacing
			}
		}
	}
}
