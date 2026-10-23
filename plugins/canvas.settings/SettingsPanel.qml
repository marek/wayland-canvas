import QtQuick
import WaylandCanvas.Ui
import "."

// The settings panel: canvas.conf's shortcuts, commands and gestures
// through the compositor's socket, the shell's own bar and toolbar
// through api.shell — one card of tabs. The model owns the socket and
// the one write (SettingsModel.qml); each tab is a file that reads it.
// The panel is the tab row, the card's geometry and the two paths the
// card edits. The Shortcuts list scrolls: the registry's every action is
// taller than an output.
PanelCard {
	id: root

	// Wide enough for its tabs' names on one row: six shares of 78 px hold
	// the longest of them ("Shortcuts") without eliding.
	panelWidth: 488

	SettingsModel {
		id: settingsModel
		api: root.api
		open: root.open
	}

	// Both files the panel edits, by path: the compositor's and the
	// shell's own. Two fixed-height lines, like every metric the card's
	// geometry is computed from.
	readonly property int footerHeight: 2 * UiTheme.detailHeight + 2

	// The most a scrollable list may take: the tall card is clamped to
	// the output's top, so what is left beside the tabs, the separator
	// and the footer is every pixel the list has. Fixed metrics only —
	// the card's geometry must not drift with the font.
	readonly property real listMaxHeight: screenH - 2 * UiTheme.edgeMargin
		- 2 * UiTheme.panelPadding - UiTheme.rowHeight - 3 * UiTheme.panelSpacing
		- footerHeight - 1

	// The tabs, in the order the row shows them. The Plugins tab is there
	// while any plugin declares settings (docs/intents/shell/plugins.md).
	readonly property var tabs: [
		{id: "shortcuts", label: "Shortcuts", component: shortcutsTab},
		{id: "gestures", label: "Gestures", component: gesturesTab},
		{id: "session", label: "Session", component: sessionTab},
		{id: "toolbar", label: "Toolbar", component: toolbarTab},
		{id: "bar", label: "Bar", component: barTab},
		{id: "plugins", label: "Plugins", component: pluginsTab,
			shown: api.shell.pluginSettingsPages.length > 0},
	]
	readonly property var shownTabs: tabs.filter(function(each) {
		return each.shown !== false
	})

	property string tab: "shortcuts"
	readonly property var currentTab: shownTabs.find(function(each) {
		return each.id === tab
	}) || shownTabs[0]

	PanelTabs {
		tabs: root.shownTabs
		current: root.currentTab.id
		onChosen: function(id) { root.tab = id }
	}

	Loader {
		width: parent.width
		sourceComponent: root.currentTab.component
	}

	Component {
		id: shortcutsTab

		ShortcutsTab {
			model: settingsModel
			maxHeight: root.listMaxHeight
		}
	}

	Component {
		id: gesturesTab

		GesturesTab {
			model: settingsModel
		}
	}

	Component {
		id: sessionTab

		SessionTab {
			model: settingsModel
		}
	}

	Component {
		id: toolbarTab

		ToolbarTab {
			shell: root.api.shell
		}
	}

	Component {
		id: barTab

		BarTab {
			shell: root.api.shell
		}
	}

	Component {
		id: pluginsTab

		PluginsTab {
			shell: root.api.shell
			maxHeight: root.listMaxHeight
		}
	}

	PanelSeparator {}

	Column {
		width: parent.width
		spacing: 2

		PanelText {
			height: UiTheme.detailHeight
			verticalAlignment: Text.AlignVCenter
			width: parent.width
			elide: Text.ElideRight
			dim: true
			font.pixelSize: UiTheme.fontSmall
			text: settingsModel.settings && settingsModel.settings.path
				? settingsModel.settings.path : ""
		}

		PanelText {
			height: UiTheme.detailHeight
			verticalAlignment: Text.AlignVCenter
			width: parent.width
			elide: Text.ElideRight
			dim: true
			font.pixelSize: UiTheme.fontSmall
			text: api.shell.configPath
		}
	}
}
