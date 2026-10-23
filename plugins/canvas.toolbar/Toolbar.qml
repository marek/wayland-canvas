import QtQuick
import WaylandCanvas.Ui
import "."

// One overview toolbar: the strip of every loaded plugin's toolbar buttons,
// api.shell.toolbarButtons, on the edge toolbar.position names
// (docs/intents/shell/toolbar.md). No buttons means no toolbar. EdgeStrip
// draws the strip; this file is the buttons, the flyout and what a click does.
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

	readonly property var buttons: api.shell.toolbarButtons

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

	// The button whose flyout is open, null when none is; the button whose
	// label is shown on hover.
	property var flyoutButton: null
	property var hoverButton: null

	// Every button routes its click here: a menu button toggles its flyout,
	// any other click folds the open flyout (it is outside one) and runs the
	// action or toggles the panel.
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
