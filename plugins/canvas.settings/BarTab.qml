import QtQuick
import WaylandCanvas.Ui

// The Bar tab: every bar widget any plugin contributes, by section, each
// with a switch that hides or shows it through api.shell.
Column {
	id: root

	required property var shell // api.shell

	width: parent.width
	spacing: UiTheme.panelSpacing

	Repeater {
		model: ["left", "center", "right"]

		Column {
			id: section
			required property string modelData
			width: parent.width
			spacing: UiTheme.panelSpacing

			PanelSectionHeader {
				text: section.modelData === "left" ? "Left"
					: section.modelData === "center" ? "Center" : "Right"
			}

			Repeater {
				model: root.shell.barWidgets.filter(function(w) {
					return w.section === section.modelData
				})

				PanelSwitchRow {
					required property var modelData
					label: modelData.pluginName
					detail: modelData.description
					marked: modelData.hidden
					clickable: false
					checked: !modelData.hidden
					onToggled: root.shell.setBarHidden(modelData.name, !modelData.hidden)
				}
			}
		}
	}
}
