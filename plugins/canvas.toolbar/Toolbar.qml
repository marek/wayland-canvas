import Quickshell
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui
import "."

// One overview toolbar: while its screen shows the overview — the
// compositor's state, matched to the screen by output name — a vertical
// strip of every loaded plugin's toolbar buttons slides in on the right
// edge, 8 px from it, vertically centred, above the canvas and below the
// panels; it slides out as the screen leaves. The host hands the surface
// the ordered buttons (the config's toolbar.order, then plugin id and
// manifest order); a button naming none or more than one of action,
// panel and menu is skipped with a line, and no buttons at all means no
// toolbar. The window spans the whole output so a menu button's flyout
// can expand out of the strip to the left of its button and a click
// outside it can be caught, but only the strip — and, while a flyout is
// open, everything below the bar — is masked in, so the overview keeps
// its pointer everywhere else. The toolbar never takes the keyboard:
// the overview's keys, Escape above all, keep working while it shows.
PanelWindow {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null // the output this toolbar is on

	screen: modelData

	// The strip's own metrics: a 44 px square per button, the shell's
	// edge margin from the right edge and before a flyout or panel, and
	// how long the slide takes.
	readonly property int buttonSize: 44
	readonly property int flyoutWidth: 200
	readonly property int slideMs: 200

	color: "transparent"
	exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation
	anchors {
		top: true
		bottom: true
		left: true
		right: true
	}
	WlrLayershell.namespace: "wayland-canvas-toolbar"
	WlrLayershell.layer: WlrLayer.Top // above the canvas, below the panels
	WlrLayershell.keyboardFocus: WlrKeyboardFocus.None // never the keyboard

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

	// How far out the strip sits: in place in the overview, a whole
	// strip-and-margin past the edge otherwise, so it leaves the screen
	// entirely.
	readonly property real hiddenSlide: buttonSize + UiTheme.edgeMargin
	property real slide: overview ? 0 : hiddenSlide
	Behavior on slide {
		NumberAnimation {
			id: slideAnim
			duration: root.slideMs
			easing.type: Easing.OutCubic
		}
	}

	// Mapped only while there is something to show: this screen's
	// overview, or the slide out still finishing.
	visible: buttons.length > 0 && (overview || slideAnim.running)

	// Input: the strip alone, so the overview keeps its pointer; while a
	// flyout is open, everything below the bar, so a click outside the
	// flyout lands here and folds it (the bar keeps its own clicks).
	mask: Region {
		x: root.flyoutButton != null ? 0 : strip.x
		y: root.flyoutButton != null ? UiTheme.barHeight : strip.y
		width: root.flyoutButton != null ? root.width : strip.width
		height: root.flyoutButton != null ? root.height - UiTheme.barHeight
			: strip.height
	}

	// ---- the strip and its buttons ----

	// A click that is neither a button nor a flyout row: outside, so the
	// flyout folds.
	MouseArea {
		anchors.fill: parent
		enabled: root.flyoutButton != null
		onClicked: root.foldFlyout()
	}

	Item {
		id: strip

		x: root.width - UiTheme.edgeMargin - root.buttonSize + root.slide
		y: (root.height - height) / 2
		width: root.buttonSize
		height: column.implicitHeight

		Rectangle {
			anchors.fill: parent
			color: UiTheme.background
		}

		Column {
			id: column
			spacing: 0
			anchors.horizontalCenter: parent.horizontalCenter
		}
	}

	Component {
		id: buttonComponent
		ToolbarButton {}
	}

	Component.onCompleted: {
		for (var i = 0; i < buttons.length; i++) {
			buttonComponent.createObject(column, {spec: buttons[i], surface: root})
		}
	}

	// ---- what a click does ----

	// The button whose flyout is open, null when none is.
	property var flyoutButton: null

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

	Flyout {
		id: flyout
		surface: root
		api: root.api
		button: root.flyoutButton
		visible: root.flyoutButton != null
	}

	// Leaving the overview folds the flyout and closes a panel this
	// toolbar opened, along with the slide out.
	onOverviewChanged: if (!overview) {
		foldFlyout()
		if (api != null && api.panels != null) {
			var anchor = api.panels.openAnchor
			if (anchor != null && anchor.QsWindow.window === root) {
				api.panels.close()
			}
		}
	}
}
