import QtQuick
import WaylandCanvas.Ui
import "."

// The Plugins tab: every setting any plugin declares, drawn from its
// declaration alone (docs/intents/shell/plugins.md). The window knows no
// plugin by name; one tab holds them all, where a tab per plugin would
// grow the row without bound and the row's width is the card's.
Flickable {
	id: root

	required property var shell // api.shell
	required property real maxHeight // the most the list may take

	width: parent.width
	implicitHeight: Math.min(contentHeight, maxHeight)
	height: implicitHeight
	contentWidth: width
	contentHeight: column.implicitHeight
	clip: true

	// Every declared setting of every plugin, flattened into rows with the
	// headers that separate them: {header} or {entry, pluginId}. A page
	// built from one model keeps the list scrollable as one.
	readonly property var rows: {
		var out = []
		var pages = shell.pluginSettingsPages
		for (var i = 0; i < pages.length; i++) {
			var page = pages[i]
			for (var g = 0; g < page.groups.length; g++) {
				var group = page.groups[g]
				out.push({
					header: group.name === page.name
						? page.name : page.name + " \u00b7 " + group.name,
				})
				for (var e = 0; e < group.entries.length; e++) {
					out.push({entry: group.entries[e], pluginId: page.id})
				}
			}
		}
		return out
	}

	Column {
		id: column
		width: parent.width
		spacing: UiTheme.panelSpacing

		Repeater {
			model: root.rows

			Loader {
				required property var modelData

				width: column.width
				sourceComponent: modelData.header !== undefined
					? headerRow : settingRow

				property var row: modelData
			}
		}
	}

	Component {
		id: headerRow

		PanelSectionHeader {
			text: parent.row.header
		}
	}

	Component {
		id: settingRow

		PluginSettingRow {
			width: parent.width
			entry: parent.row.entry
			pluginId: parent.row.pluginId
			shell: root.shell
		}
	}
}
