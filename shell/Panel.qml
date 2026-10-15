import Quickshell
import Quickshell.Wayland
import QtQuick
import "."

// One indicator panel: a full-output overlay layer surface holding a card
// centred under its indicator and clamped inside the output. A click
// outside the card closes the panel (the overlay below the bar strip), so
// does Escape and a second click of its indicator (the bar strip is masked
// out of the surface, so those clicks reach the bar); Panels owns the
// one-at-a-time rule. The card is a layer surface rather than an xdg
// popup: the Qt platform refuses to attach a popup to a layer-shell
// parent. It asks for the keyboard on demand, which the compositor gives a
// surface above the windows as it maps, so a text field in a panel
// receives typing without a click first and Escape is caught here before
// any field sees it.
PanelWindow {
	id: root

	property var theme: null
	property var anchorItem: null // the bar widget the panel hangs from
	// Names the surface so the scenario suite can tell the panels apart;
	// the fixed width per panel is the other distinguishing mark.
	property string panelName: ""
	property int panelWidth: 240

	default property alias content: column.data
	property alias spacing: column.spacing
	property bool open: false

	visible: open
	color: "transparent"
	exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation

	readonly property var anchorWindow: anchorItem ? anchorItem.QsWindow.window : null
	screen: anchorWindow ? anchorWindow.screen : null

	WlrLayershell.namespace: "wayland-canvas-panel-" + panelName
	WlrLayershell.layer: WlrLayer.Overlay
	WlrLayershell.keyboardFocus: open ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

	anchors {
		top: true
		bottom: true
		left: true
		right: true
	}

	// Everything below the bar strip is this panel's to catch; the strip
	// itself stays the bar's, which is what makes a second click of the
	// indicator toggle the panel closed.
	mask: Region {
		y: root.cardY
		width: root.screenW
		height: root.screenH - root.cardY
	}

	function showPanel() {
		open = true
	}

	function close() {
		open = false
	}

	// The compositor hides the surface behind `open`'s back if it is ever
	// forced off the screen: resync, and let Panels forget this panel.
	onVisibleChanged: if (!visible && open) {
		open = false
		Panels.released(root)
	}

	readonly property real screenW: anchorWindow && anchorWindow.screen
		? anchorWindow.screen.width : 1280
	readonly property real screenH: anchorWindow && anchorWindow.screen
		? anchorWindow.screen.height : 720
	readonly property real barH: anchorWindow ? anchorWindow.height : 26

	// The card: centred under the indicator, kept inside the output. The
	// watcher's transform is the reactive dependency that re-runs the
	// position when the bar lays its widgets out or moves them — a bare
	// mapToItem binding would only see the first, pre-layout position.
	TransformWatcher {
		id: anchorWatcher
		a: root.anchorWindow ? root.anchorWindow.contentItem : null
		b: root.anchorItem
	}

	readonly property real cardX: {
		anchorWatcher.transform
		var margin = theme ? theme.edgeMargin : 8
		var centre = anchorItem ? anchorItem.mapToItem(null, anchorItem.width / 2, 0).x : 0
		return Math.round(Math.max(margin, Math.min(centre - panelWidth / 2,
			screenW - panelWidth - margin)))
	}
	readonly property real cardY: barH

	Item {
		anchors.fill: parent

		// The dismissal overlay: a click anywhere the card is not.
		MouseArea {
			anchors.fill: parent
			onClicked: root.close()
		}

		Rectangle {
			id: card

			x: root.cardX
			y: root.cardY
			width: root.panelWidth
			height: column.implicitHeight + 2 * (root.theme ? root.theme.panelPadding : 10)
			color: root.theme ? root.theme.background : "#1a1b26"

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
				x: root.theme ? root.theme.panelPadding : 10
				y: root.theme ? root.theme.panelPadding : 10
				width: parent.width - 2 * x
				spacing: root.theme ? root.theme.panelSpacing : 6
			}
		}
	}
}
