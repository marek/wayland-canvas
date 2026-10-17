import Quickshell
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui
import "."

// One overview toolbar: while its screen shows the overview — the
// compositor's state, matched to the screen by output name — the strip
// of every loaded plugin's toolbar buttons slides in on the edge the
// config's toolbar.position names (right by default; a column on the
// left and right, a row on the top and bottom, 8 px from the edge, the
// top row below the bar) and slides out as the screen leaves. A new
// edge applies at once: the strip's anchors follow the live config. The
// host hands the surface the ordered buttons (the config's
// toolbar.order, then plugin id and manifest order); a button naming
// none or more than one of action, panel and menu is skipped with a
// line, and no buttons at all means no toolbar.
//
// Two surfaces: the strip is its own layer surface, anchored to its
// edge and sized the strip plus a pocket that overhangs the screen edge
// — the strip slides into the pocket as the screen leaves the overview,
// so the window's geometry never changes, and the compositor's placed
// box still names the strip (a bottom-anchored window's top is the
// strip's top: what the overview's shelf and help line keep clear of)
// — while the hovered button's label and a menu button's flyout draw
// on a companion surface spanning the output, so both can expand past
// the strip toward the screen's centre. That surface's input is the
// open flyout's alone. Neither surface ever takes the keyboard: the
// overview's keys, Escape above all, keep working while the toolbar
// shows.
Item {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null // the output this toolbar is on

	// The strip's own metrics: a 44 px square per button, the shell's
	// edge margin from the edge and before a flyout or panel, and how
	// long the slide takes.
	readonly property int buttonSize: 44
	readonly property int flyoutWidth: 200
	readonly property int slideMs: 200
	readonly property int edgeMargin: UiTheme.edgeMargin

	// The host item, which holds the live config: api.shell where the
	// host hands itself out by that name, the plugin api's back-link
	// otherwise — the same item either way.
	readonly property var shell: {
		if (api == null) {
			return null
		}
		if (api.shell !== undefined) {
			return api.shell
		}
		return api.plugin != null ? api.plugin.host : null
	}

	// The edge the config names — a column on the left and right, a row
	// on the top and bottom — right when it names none of them or
	// something else: a hand edit can hold anything.
	readonly property string edge: {
		var config = shell != null ? shell.config : null
		var named = config != null && config.toolbar != null
			? config.toolbar.position : ""
		return named === "left" || named === "top" || named === "bottom" ? named : "right"
	}
	readonly property bool vertical: edge === "left" || edge === "right"

	// ---- which buttons, and when ----

	// The buttons worth showing: everything the registry holds but the
	// ones naming none or more than one of action, panel and menu.
	readonly property var buttons: validButtons()

	function validButtons() {
		var out = []
		var all = registry != null ? registry.toolbarButtons : []
		for (var i = 0; i < all.length; i++) {
			var button = all[i]
			var kinds = (button.action !== "" ? 1 : 0)
				+ (button.panel !== "" ? 1 : 0)
				+ (button.menu.length > 0 ? 1 : 0)
			if (kinds !== 1) {
				console.warn("[plugins] skipping toolbar button " + button.name
					+ ": it names " + (kinds === 0
						? "no action, panel or menu"
						: "more than one of action, panel and menu"))
				continue
			}
			out.push(button)
		}
		return out
	}

	// Whether this screen shows the overview, from the compositor's
	// kept-up-to-date state.
	readonly property bool overview: {
		var outputs = api != null && api.compositor != null
			? api.compositor.state.outputs : []
		for (var i = 0; i < outputs.length; i++) {
			if (outputs[i].name === modelData.name) {
				return outputs[i].overview === true
			}
		}
		return false
	}

	// ---- the slide ----

	// How far the strip sits from its settled place: none in the
	// overview; a whole pocket otherwise, so it leaves the screen
	// entirely — past the screen edge on the left, right and bottom,
	// past the screen's top on the top edge (the row sits below the
	// bar). The strip's window never moves: its geometry is fixed from
	// the anchors and margins below, and the slide moves only the
	// content inside it, so the surface is never asked to reconfigure
	// mid-animation (a fully off-screen surface stops getting frames,
	// and a slide it carried would strand it there).
	readonly property int nearEdge: edge === "top"
		? UiTheme.barHeight + edgeMargin : edgeMargin
	// The pocket the strip slides into: its own square plus the distance
	// from the screen edge to its settled place.
	readonly property int pocket: nearEdge + buttonSize
	readonly property real hiddenSlide: -pocket
	property real slide: overview ? 0 : hiddenSlide
	Behavior on slide {
		NumberAnimation {
			id: slideAnim
			duration: root.slideMs
			easing.type: Easing.OutCubic
		}
	}

	// ---- the strip's place in output coordinates ----

	// The strip's settled box, in output coordinates: what the compositor
	// places from the same anchors and margins the strip commits. The
	// top edge's margin is measured from the output's top and carries the
	// built-in bar's height (the same metric the bar sizes itself by), so
	// the row sits below the bar.
	readonly property int stripLength: buttons.length * buttonSize
	// The app bar on this same edge shares the strip: the two place
	// themselves from StripLayout with both lengths, the buttons first and
	// the apps after the separator (docs/intents/shell/app-bar.md). Neither
	// surface knows the other: this length comes from the shell config and
	// the compositor's window list, which the app bar reads too.
	readonly property int appsLength: {
		var section = shell != null && shell.config != null
			? shell.config.appbar : null
		if (section != null && section.enabled === false) {
			return 0
		}
		var named = section != null && section.position !== undefined
			? section.position : ""
		var appEdge = named === "right" || named === "top" || named === "bottom"
			? named : "left"
		if (appEdge !== edge) {
			return 0
		}
		var windows = api != null && api.compositor != null
			? (api.compositor.state.windows || []) : []
		var seen = ({})
		var apps = 0
		for (var i = 0; i < windows.length; i++) {
			var key = windows[i].appId !== ""
				? windows[i].appId : ("window:" + windows[i].id)
			if (seen[key] === undefined) {
				seen[key] = true
				apps++
			}
		}
		return apps * buttonSize
	}
	readonly property int along: StripLayout.toolbarStart(
		vertical ? modelData.height : modelData.width, stripLength, appsLength)
	readonly property real stripX: edge === "right"
		? modelData.width - edgeMargin - buttonSize
		: edge === "left" ? edgeMargin
		: along
	readonly property real stripY: edge === "bottom"
		? modelData.height - edgeMargin - buttonSize
		: edge === "top" ? UiTheme.barHeight + edgeMargin
		: along

	// The strip's live displacement from its settled place while it
	// slides: off the screen, past the edge.
	readonly property real slideDx: edge === "right" ? -slide : edge === "left" ? slide : 0
	readonly property real slideDy: edge === "bottom" ? -slide : edge === "top" ? slide : 0

	// A button's square, in output coordinates: the buttons fill the
	// strip in order, one square each. The chrome's label and flyout and
	// the panels' anchors all place themselves from this.
	function buttonBox(index) {
		if (index < 0) {
			return null
		}
		if (vertical) {
			return {
				x: stripX + slideDx,
				y: stripY + slideDy + index * buttonSize,
				width: buttonSize,
				height: buttonSize,
			}
		}
		return {
			x: stripX + slideDx + index * buttonSize,
			y: stripY + slideDy,
			width: buttonSize,
			height: buttonSize,
		}
	}

	// ---- the strip ----

	PanelWindow {
		id: strip

		screen: root.modelData
		color: "transparent"
		exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation
		// Mapped only while there is something to show: this screen's
		// overview, or the slide out still finishing.
		visible: root.buttons.length > 0 && (root.overview || slideAnim.running)
		WlrLayershell.namespace: "wayland-canvas-toolbar"
		WlrLayershell.layer: WlrLayer.Top // above the canvas, below the panels
		WlrLayershell.keyboardFocus: WlrKeyboardFocus.None // never the keyboard

		// Anchored to the edge alone (the bottom-edge anchor is what
		// tells the compositor the shelf must keep clear of it), with the
		// one fixed margin that overhangs the screen edge by a button
		// square: the window is the strip plus the pocket that holds it
		// off-screen while it slides, so the compositor's placed box
		// names the strip's edge — a bottom-anchored window's top is the
		// strip's top. On the top edge the pocket reaches past the
		// screen's top, over the bar's area: transparent and without
		// input there, so the bar keeps its clicks.
		// Its edge, and the leading edge along it: the margin along the edge
		// is where StripLayout puts this part, which is the strip centred on
		// its own, or its share of the strip the app bar is on too.
		anchors.right: root.edge === "right"
		anchors.left: root.edge === "left" || !root.vertical
		anchors.top: root.edge === "top" || root.vertical
		anchors.bottom: root.edge === "bottom"
		WlrLayershell.margins.left: root.edge === "left" ? -root.buttonSize
			: root.vertical ? 0 : root.along
		WlrLayershell.margins.right: root.edge === "right" ? -root.buttonSize : 0
		WlrLayershell.margins.top: root.edge === "top"
			? root.nearEdge - root.pocket
			: root.vertical ? root.along : 0
		WlrLayershell.margins.bottom: root.edge === "bottom" ? -root.buttonSize : 0

		// The free dimension of the anchored edge carries the strip's
		// length; the anchored one is the strip plus its pocket wide.
		implicitWidth: root.vertical
			? root.buttonSize + root.pocket : root.stripLength
		implicitHeight: root.vertical
			? root.stripLength : root.buttonSize + root.pocket

		// Input is the strip's alone, wherever the slide has it: the
		// pocket is off-screen, and a slid-out strip takes no clicks.
		mask: Region {
			x: content.x
			y: content.y
			width: root.vertical ? root.buttonSize : root.stripLength
			height: root.vertical ? root.stripLength : root.buttonSize
		}

		// The strip itself, sliding inside the window: settled on the
		// side away from the screen edge, hidden in the pocket past it.
		Item {
			id: content
			x: root.edge === "right" ? -root.slide
				: root.edge === "left" ? root.pocket + root.slide : 0
			y: root.edge === "bottom" ? -root.slide
				: root.edge === "top" ? root.pocket + root.slide : 0
			width: root.vertical ? root.buttonSize : root.stripLength
			height: root.vertical ? root.stripLength : root.buttonSize

			// A card like the panels: on the light canvas, which is the
			// background colour, a background-coloured strip would
			// vanish.
			Rectangle {
				anchors.fill: parent
				color: UiTheme.panel
				border.width: 1
				border.color: UiTheme.border
			}

			// The buttons place themselves (ToolbarButton binds x and y
			// to this surface's layout), so a live edge change moves them
			// with the strip.
			Item {
				id: buttonArea
				anchors.fill: parent
			}
		}
	}

	// ---- the chrome: the label and the flyout ----

	PanelWindow {
		id: chrome

		screen: root.modelData
		color: "transparent"
		exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation
		visible: root.buttons.length > 0 && root.overview
		WlrLayershell.namespace: "wayland-canvas-toolbar-chrome"
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

		// Input is the open flyout's alone: nothing while none is (the
		// pointer reaches the overview and the strip below), everything
		// under the bar but the strip itself while one is — a click
		// outside the flyout folds it, and whichever of the two surfaces
		// ended up on top, the buttons keep their clicks. The four
		// regions are the below-bar area cut around the strip.
		mask: Region {
			Region { // the band above the strip
				x: 0
				y: UiTheme.barHeight
				width: root.catching ? root.modelData.width : 0
				height: Math.max(0, chrome.stripTop - UiTheme.barHeight)
			}
			Region { // beside the strip, to the output's left edge
				x: 0
				y: chrome.stripTop
				width: root.catching ? Math.max(0, chrome.stripLeft) : 0
				height: Math.max(0, chrome.stripBottom - chrome.stripTop)
			}
			Region { // beside the strip, to the output's right edge
				x: chrome.stripRight
				y: chrome.stripTop
				width: root.catching
					? Math.max(0, root.modelData.width - chrome.stripRight) : 0
				height: Math.max(0, chrome.stripBottom - chrome.stripTop)
			}
			Region { // the band below the strip
				x: 0
				y: chrome.stripBottom
				width: root.catching ? root.modelData.width : 0
				height: Math.max(0, root.modelData.height - chrome.stripBottom)
			}
		}

		// The strip's live box in this window's (the output's)
		// coordinates, rounded like the compositor places the surface.
		readonly property int stripLeft: Math.round(root.stripX + root.slideDx)
		readonly property int stripTop: Math.round(root.stripY + root.slideDy)
		readonly property int stripRight: stripLeft
			+ (root.vertical ? root.buttonSize : root.stripLength)
		readonly property int stripBottom: stripTop
			+ (root.vertical ? root.stripLength : root.buttonSize)

		Item {
			anchors.fill: parent

			// A click that is neither a flyout row nor on the strip:
			// outside, so the flyout folds.
			MouseArea {
				anchors.fill: parent
				enabled: root.flyoutButton != null
				onClicked: root.foldFlyout()
			}

			Flyout {
				id: flyout
				surface: root
				api: root.api
				button: root.flyoutButton
				visible: root.flyoutButton != null
			}

			// The hovered button's label: on a chip beside it, toward the
			// screen's centre, so it reads over whatever the overview is
			// showing.
			Rectangle {
				id: label

				readonly property var box: root.hoverButton != null
					? root.buttonBox(root.hoverButton.index) : null
				readonly property int gap: 8
				readonly property int pad: 6

				visible: box != null && root.hoverButton.spec != null
					&& root.hoverButton.spec.label !== ""
				x: box == null ? 0 : root.edge === "right"
					? box.x - gap - width
					: root.edge === "left" ? box.x + box.width + gap
					: box.x + box.width / 2 - width / 2
				y: box == null ? 0 : root.edge === "top"
					? box.y + box.height + gap
					: root.edge === "bottom" ? box.y - gap - height
					: box.y + box.height / 2 - height / 2
				width: labelText.implicitWidth + 2 * pad
				height: labelText.implicitHeight + 2 * pad
				radius: 4
				color: UiTheme.background

				PanelText {
					id: labelText
					anchors.centerIn: parent
					text: root.hoverButton != null && root.hoverButton.spec != null
						? root.hoverButton.spec.label : ""
				}
			}
		}
	}

	Component {
		id: buttonComponent
		ToolbarButton {}
	}

	Component.onCompleted: {
		for (var i = 0; i < buttons.length; i++) {
			buttonComponent.createObject(buttonArea, {spec: buttons[i], surface: root, index: i})
		}
	}

	// ---- what a click does ----

	// The button whose flyout is open, null when none is; the button
	// whose label is shown on hover (its chip follows it on the chrome).
	property var flyoutButton: null
	property var hoverButton: null
	// Whether the chrome must catch clicks for the open flyout (its mask
	// and its outside-click catcher follow this).
	readonly property bool catching: flyoutButton != null

	// Every button routes its click here: a menu button toggles its
	// flyout (a second click folds it), any other click folds whatever
	// flyout is open — it is outside one — and then runs its action or
	// toggles its panel through the common trigger.
	function buttonActivated(button) {
		var spec = button.spec
		if (spec.menu.length > 0) {
			toggleFlyout(button)
			return
		}
		foldFlyout()
		if (spec.action !== "") {
			api.actions.run(spec.action, spec.args)
		} else if (spec.panel !== "") {
			api.panels.toggle(spec.panel, button)
		}
	}

	function toggleFlyout(button) {
		flyoutButton = flyoutButton === button ? null : button
	}

	function foldFlyout() {
		flyoutButton = null
	}

	// Leaving the overview folds the flyout and closes a panel this
	// toolbar opened, along with the slide out.
	onOverviewChanged: if (!overview) {
		foldFlyout()
		hoverButton = null
		if (api != null && api.panels != null) {
			var anchor = api.panels.openAnchor
			if (anchor != null && anchor.QsWindow.window === strip) {
				api.panels.close()
			}
		}
	}
}
