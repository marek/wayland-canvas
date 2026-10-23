import Quickshell
import Quickshell.Wayland
import QtQuick
import "."

// One strip of squares on an edge of the overview, for the toolbar and the app
// bar (docs/intents/shell/toolbar.md, app-bar.md): it slides in on `edge` while
// its screen shows the overview and out as the screen leaves, and StripLayout
// places the strips that share an edge as one. The strip is a layer surface of
// its own; the chrome, a second one spanning the output, draws the hover label
// and the plugin's flyouts. Neither takes the keyboard.
Item {
	id: root

	required property var api
	required property var modelData // the output this strip is on

	// The edge, from api.shell, and the layer namespace of the strip; the
	// chrome's is the same with "-chrome".
	required property string edge
	required property string namespace
	property int count: 0 // the squares
	property bool switchedOn: true // false leaves no surface at all
	// Its place among the strips on a shared edge: the first comes first.
	property int order: 0

	// The square the pointer is over, -1 for none, and its label.
	property int hoverIndex: -1
	property string label: ""
	// Whether a flyout is open: the chrome then takes every click outside
	// the strip, and `dismissed` tells the plugin to fold it.
	property bool catching: false
	signal dismissed()

	// The squares go in the strip, the flyouts on the chrome.
	default property alias squares: grid.data
	property alias flyouts: flyoutLayer.data

	readonly property int squareSize: 44
	readonly property int slideMs: 200
	readonly property int edgeMargin: UiTheme.edgeMargin
	readonly property bool vertical: edge === "left" || edge === "right"
	// The strip's length along its edge: what it publishes to StripLayout.
	readonly property int length: switchedOn ? count * squareSize : 0

	Component.onCompleted: StripLayout.join(root)
	Component.onDestruction: StripLayout.leave(root)

	// Whether this screen shows the overview, from the compositor's
	// kept-up-to-date state.
	readonly property bool overview: {
		var outputs = api.compositor != null ? api.compositor.state.outputs : []
		for (var i = 0; i < outputs.length; i++) {
			if (outputs[i].name === modelData.name) {
				return outputs[i].overview === true
			}
		}
		return false
	}

	// How far the strip sits from its settled place: none in the overview, a
	// whole pocket (off the screen) otherwise. Only the content slides: a
	// surface moved fully off-screen stops getting frames and could not
	// slide back, so the window stays put.
	readonly property int nearEdge: edge === "top"
		? UiTheme.barHeight + edgeMargin : edgeMargin
	// The pocket the strip slides into: its own square plus the distance
	// from the screen edge to its settled place.
	readonly property int pocket: nearEdge + squareSize
	property real slide: overview ? 0 : -pocket
	Behavior on slide {
		NumberAnimation {
			id: slideAnim
			duration: root.slideMs
			easing.type: Easing.OutCubic
		}
	}

	// Along the edge, from StripLayout: the strip centred on its own, or its
	// share of the strip the others on the edge are part of.
	readonly property int along: StripLayout.start(root)
	// The settled box: what the compositor places from the same anchors and
	// margins the strip commits. The top edge's margin is measured from the
	// output's top and carries the bar's height, so the row sits below it.
	readonly property real stripX: edge === "right"
		? modelData.width - edgeMargin - squareSize
		: edge === "left" ? edgeMargin
		: along
	readonly property real stripY: edge === "bottom"
		? modelData.height - edgeMargin - squareSize
		: edge === "top" ? nearEdge
		: along
	// The strip's live displacement from its settled place while it
	// slides: off the screen, past the edge.
	readonly property real slideDx: edge === "right" ? -slide : edge === "left" ? slide : 0
	readonly property real slideDy: edge === "bottom" ? -slide : edge === "top" ? slide : 0
	// The strip's live box in the chrome's (the output's) coordinates,
	// rounded like the compositor places the surface.
	readonly property int stripLeft: Math.round(stripX + slideDx)
	readonly property int stripTop: Math.round(stripY + slideDy)
	readonly property int stripRight: stripLeft + (vertical ? squareSize : length)
	readonly property int stripBottom: stripTop + (vertical ? length : squareSize)

	// A square's box, in output coordinates: the squares fill the strip in
	// order. The label, the flyouts and the panels' anchors place
	// themselves from this.
	function squareBox(index) {
		if (index < 0) {
			return null
		}
		return {
			x: stripLeft + (vertical ? 0 : index * squareSize),
			y: stripTop + (vertical ? index * squareSize : 0),
			width: squareSize,
			height: squareSize,
		}
	}

	// The corner of a `width` x `height` card beside `box`, toward the
	// screen's centre, kept inside the output (StripLayout.placeBeside).
	function placeBeside(box, width, height, centred, gap) {
		return StripLayout.placeBeside(edge, box, width, height, modelData, gap, centred)
	}

	// Whether `item` is drawn by this strip's own window.
	function owns(item) {
		return item != null && item.QsWindow.window === strip
	}

	PanelWindow {
		id: strip

		screen: root.modelData
		color: "transparent"
		exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation
		// Mapped only while there is something to show: this screen's
		// overview, or the slide out still finishing.
		visible: root.switchedOn && root.count > 0
			&& (root.overview || slideAnim.running)
		WlrLayershell.namespace: root.namespace
		WlrLayershell.layer: WlrLayer.Top // above the canvas, below the panels
		WlrLayershell.keyboardFocus: WlrKeyboardFocus.None // never the keyboard

		// Anchored to its edge and to the leading edge along it: the margin
		// along the edge places the strip where StripLayout says, and the
		// margin on the edge itself overhangs by a square, so the window is
		// the strip plus its pocket and the compositor's placed box names the
		// strip's edge (what the shelf keeps clear of). On the top edge the
		// pocket reaches over the bar, transparent and without input.
		anchors.right: root.edge === "right"
		anchors.left: root.edge === "left" || !root.vertical
		anchors.top: root.edge === "top" || root.vertical
		anchors.bottom: root.edge === "bottom"
		WlrLayershell.margins.left: root.edge === "left" ? -root.squareSize
			: root.vertical ? 0 : root.along
		WlrLayershell.margins.right: root.edge === "right" ? -root.squareSize : 0
		WlrLayershell.margins.top: root.edge === "top"
			? root.nearEdge - root.pocket
			: root.vertical ? root.along : 0
		WlrLayershell.margins.bottom: root.edge === "bottom" ? -root.squareSize : 0

		// The free dimension of the anchored edge carries the strip's
		// length; the anchored one is the strip plus its pocket wide.
		implicitWidth: root.vertical ? root.squareSize + root.pocket : root.length
		implicitHeight: root.vertical ? root.length : root.squareSize + root.pocket

		// Input is the strip's alone, wherever the slide has it: the pocket
		// is off-screen, and a slid-out strip takes no clicks.
		mask: Region {
			x: content.x
			y: content.y
			width: content.width
			height: content.height
		}

		// The strip itself, sliding inside the window: settled on the side
		// away from the screen edge, hidden in the pocket past it.
		Item {
			id: content
			x: root.edge === "right" ? -root.slide
				: root.edge === "left" ? root.pocket + root.slide : 0
			y: root.edge === "bottom" ? -root.slide
				: root.edge === "top" ? root.pocket + root.slide : 0
			width: root.vertical ? root.squareSize : root.length
			height: root.vertical ? root.length : root.squareSize

			// A card like the panels: on the light canvas, which is the
			// background colour, a background-coloured strip would vanish.
			Rectangle {
				anchors.fill: parent
				color: UiTheme.panel
				border.width: 1
				border.color: UiTheme.border
			}

			// The separator between a strip before this one on the edge and
			// this one: a hairline across the strip in the gap the two
			// leave, drawn on this side of it.
			Rectangle {
				visible: StripLayout.hasLeader(root)
				color: UiTheme.border
				width: root.vertical ? parent.width - 16 : 1
				height: root.vertical ? 1 : parent.height - 16
				x: root.vertical ? 8 : -Math.round(StripLayout.separator / 2)
				y: root.vertical ? -Math.round(StripLayout.separator / 2) : 8
			}

			// The squares lay themselves out along the edge, so a live edge
			// change moves them with the strip.
			Grid {
				id: grid
				flow: root.vertical ? Grid.TopToBottom : Grid.LeftToRight
				rows: root.vertical ? Math.max(1, root.count) : 1
				columns: root.vertical ? 1 : Math.max(1, root.count)
			}
		}
	}

	PanelWindow {
		id: chrome

		screen: root.modelData
		color: "transparent"
		exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation
		// Mapped only while a label or a flyout is up.
		visible: root.switchedOn && root.count > 0 && root.overview
			&& (root.hoverIndex >= 0 || root.catching)
		WlrLayershell.namespace: root.namespace + "-chrome"
		// Above the compositor's own overview UI as well: a flyout that
		// hangs over a bottom strip reaches the shelf and the help line,
		// which draw over the top layer.
		WlrLayershell.layer: WlrLayer.Overlay
		WlrLayershell.keyboardFocus: WlrKeyboardFocus.None // never the keyboard
		anchors {
			top: true
			bottom: true
			left: true
			right: true
		}

		// Input is the open flyout's alone: nothing while none is, everything
		// under the bar but the strip while one is, so a click outside the
		// flyout folds it and the squares keep their clicks. The four regions
		// are the below-bar area cut around the strip.
		mask: Region {
			Region { // the band above the strip
				x: 0
				y: UiTheme.barHeight
				width: root.catching ? root.modelData.width : 0
				height: Math.max(0, root.stripTop - UiTheme.barHeight)
			}
			Region { // beside the strip, to the output's left edge
				x: 0
				y: root.stripTop
				width: root.catching ? Math.max(0, root.stripLeft) : 0
				height: Math.max(0, root.stripBottom - root.stripTop)
			}
			Region { // beside the strip, to the output's right edge
				x: root.stripRight
				y: root.stripTop
				width: root.catching
					? Math.max(0, root.modelData.width - root.stripRight) : 0
				height: Math.max(0, root.stripBottom - root.stripTop)
			}
			Region { // the band below the strip
				x: 0
				y: root.stripBottom
				width: root.catching ? root.modelData.width : 0
				height: Math.max(0, root.modelData.height - root.stripBottom)
			}
		}

		Item {
			anchors.fill: parent

			// A click that is neither on a flyout nor on the strip: outside,
			// so the flyout folds.
			MouseArea {
				anchors.fill: parent
				enabled: root.catching
				onClicked: root.dismissed()
			}

			Item {
				id: flyoutLayer
				anchors.fill: parent
			}

			// The hovered square's label: a chip beside it toward the screen's
			// centre, its text elided to what fits on the screen.
			Rectangle {
				readonly property var box: root.squareBox(root.hoverIndex)
				readonly property int gap: 8
				readonly property int pad: 6
				readonly property var spot: box == null ? Qt.point(0, 0)
					: root.placeBeside(box, width, height, true, gap)

				visible: box != null && root.label !== "" && !root.catching
				x: spot.x
				y: spot.y
				width: Math.min(labelText.implicitWidth + 2 * pad,
					root.modelData.width - 2 * root.edgeMargin
						- (root.vertical ? root.squareSize + gap : 0))
				height: labelText.implicitHeight + 2 * pad
				radius: UiTheme.radius
				color: UiTheme.background

				PanelText {
					id: labelText
					anchors.centerIn: parent
					width: parent.width - 2 * parent.pad
					elide: Text.ElideRight
					horizontalAlignment: Text.AlignHCenter
					text: root.label
				}
			}
		}
	}
}
