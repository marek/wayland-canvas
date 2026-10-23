import QtQuick
import WaylandCanvas.Ui

// The Toolbar tab: the edge the overview's toolbar sits on, and the app
// bar's switch and edge, which share the page because they are the two
// strips of the overview (docs/intents/shell/app-bar.md). Both write
// through api.shell, the shell's own config.
Column {
	id: root

	required property var shell // api.shell

	width: parent.width
	spacing: UiTheme.panelSpacing

	readonly property bool appBarEnabled: shell.appBarEnabled !== false

	PanelSectionHeader {
		text: "The toolbar's edge"
	}

	ChoiceRow {
		label: "Right"
		description: "A column on the right edge"
		chosen: root.shell.toolbarPosition === "right"
		onPicked: root.shell.setToolbarPosition("right")
	}

	ChoiceRow {
		label: "Left"
		description: "A column on the left edge"
		chosen: root.shell.toolbarPosition === "left"
		onPicked: root.shell.setToolbarPosition("left")
	}

	ChoiceRow {
		label: "Top"
		description: "A row under the bar"
		chosen: root.shell.toolbarPosition === "top"
		onPicked: root.shell.setToolbarPosition("top")
	}

	ChoiceRow {
		label: "Bottom"
		description: "A row above the bottom edge"
		chosen: root.shell.toolbarPosition === "bottom"
		onPicked: root.shell.setToolbarPosition("bottom")
	}

	PanelSectionHeader {
		text: "The app bar"
	}

	PanelSwitchRow {
		label: "Show the app bar"
		detail: "Every open app on an edge of the overview"
		clickable: false
		checked: root.appBarEnabled
		onToggled: root.shell.setAppBarEnabled(!root.appBarEnabled)
	}

	ChoiceRow {
		label: "Left"
		description: "A column on the left edge"
		chosen: root.shell.appBarPosition === "left"
		onPicked: root.shell.setAppBarPosition("left")
	}

	ChoiceRow {
		label: "Right"
		description: "A column on the right edge"
		chosen: root.shell.appBarPosition === "right"
		onPicked: root.shell.setAppBarPosition("right")
	}

	ChoiceRow {
		label: "Top"
		description: "A row under the bar"
		chosen: root.shell.appBarPosition === "top"
		onPicked: root.shell.setAppBarPosition("top")
	}

	ChoiceRow {
		label: "Bottom"
		description: "A row above the bottom edge"
		chosen: root.shell.appBarPosition === "bottom"
		onPicked: root.shell.setAppBarPosition("bottom")
	}
}
