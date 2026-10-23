import Quickshell
import Quickshell.Wayland
import QtQuick
import "."

// One indicator panel: a full-output overlay layer surface holding a card
// centred under its indicator and clamped inside the output. The host's
// panel manager instantiates one per manifest panel entry, hidden, hands it
// the api, and on open the anchor item; it owns the one-at-a-time rule and
// the second-toggle close. What a panel shows it reads from its own
// plugin's state, never from the item that opened it, so a panel opened by
// an action looks as it does from its widget. A click outside the card
// (the overlay below the bar strip) and Escape close the panel here —
// `dismissed` tells the manager it went; the bar strip is masked out of
// the surface, so a second click of the icon reaches the bar and closes
// the panel through the manager's toggle. The card is a layer surface
// rather than an xdg popup: the Qt platform refuses to attach a popup to a
// layer-shell parent. It asks for the keyboard on demand, which the
// compositor gives a surface above the windows as it maps, so a text field
// in a panel receives typing without a click first and Escape is caught
// here before any field sees it.
PanelWindow {
	id: root

	property var api: null
	property var anchorItem: null // the bar widget the panel hangs from
	// The panel's manifest id, which the host sets: it names the layer
	// surface `wayland-canvas-panel-<panelName>`.
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
	WlrLayershell.keyboardFocus: open ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

	anchors {
		top: true
		bottom: true
		left: true
		right: true
	}

	// Everything but the bar strip is this panel's to catch; the strip
	// itself stays the bar's, which is what makes a second click of the
	// indicator toggle the panel closed. A toolbar button's strip lies
	// below the panel layer, so its card catches the whole output: a
	// click above or beside the card closes it, never reaching the window
	// below.
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
		? anchorWindow.screen.width : 1280
	readonly property real screenH: anchorWindow && anchorWindow.screen
		? anchorWindow.screen.height : 720
	readonly property real barH: anchorWindow ? anchorWindow.height : 26

	// The card: placed by its anchor, kept inside the output. A bar
	// widget hangs centred under its indicator. An anchor that Qt cannot
	// place from here — the overview toolbar's button, whose window is
	// only the strip — names the spot itself, in output coordinates, as
	// `panelSpot`: the strip's edge and the button's box. The card hangs
	// beside it on the strip's inner side, toward the screen's centre,
	// level with the button (StripLayout.placeBeside). The watcher's
	// transform is the reactive dependency that re-runs the position
	// when the bar lays its widgets out or moves them — a bare
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
