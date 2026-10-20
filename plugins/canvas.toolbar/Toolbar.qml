import QtQuick
import WaylandCanvas.Ui
import "."

// One overview toolbar: while its screen shows the overview, the strip of
// every loaded plugin's toolbar buttons slides in on the edge the config's
// toolbar.position names (right by default) and slides out as the screen
// leaves (docs/intents/shell/toolbar.md). The strip, its slide, its edge and
// its chrome are the UI kit's EdgeStrip; this file is what is the
// toolbar's own: the buttons, the menu flyout and what a click does. The
// host hands the surface the ordered, valid buttons (the config's
// toolbar.order, then plugin id and manifest order), and no buttons at all
// means no toolbar.
EdgeStrip {
	id: root

	edge: api.shell.toolbarPosition
	namespace: "wayland-canvas-toolbar"
	count: buttons.length
	order: 0
	hoverIndex: hoverButton != null ? hoverButton.index : -1
	label: hoverButton != null ? hoverButton.spec.label : ""
	catching: flyoutButton != null
	onDismissed: foldFlyout()

	readonly property int flyoutWidth: 200

	readonly property var buttons: registry != null ? registry.toolbarButtons : []

	Repeater {
		model: root.buttons

		ToolbarButton {
			surface: root
		}
	}

	flyouts: Flyout {
		surface: root
		api: root.api
		button: root.flyoutButton
		visible: root.flyoutButton != null
	}

	// ---- what a click does ----

	// The button whose flyout is open, null when none is; the button whose
	// label is shown on hover.
	property var flyoutButton: null
	property var hoverButton: null

	// Every button routes its click here: a menu button toggles its flyout
	// (a second click folds it), any other click folds whatever flyout is
	// open — it is outside one — and then runs its action or toggles its
	// panel through the common trigger.
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

	// Leaving the overview folds the flyout and closes a panel this toolbar
	// opened, along with the slide out.
	onOverviewChanged: if (!overview) {
		foldFlyout()
		hoverButton = null
		if (owns(api.panels.openAnchor)) {
			api.panels.close()
		}
	}
}
