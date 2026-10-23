import QtQuick
import WaylandCanvas.Ui

// One setting a plugin declared, drawn from its declaration alone
// (docs/intents/shell/plugins.md): a switch, a choice, a number or a text
// field, with its label and the line under it. A change goes straight to the
// plugin's own setting, which writes the shell config.
Column {
	id: root

	property var entry: null // {key, type, label, description, choices, ...}
	property string pluginId: ""
	property var shell: null // api.shell, which writes the setting

	spacing: UiTheme.panelSpacing

	// A number setting's range, for the slider's 0..1.
	readonly property real span: entry != null && entry.type === "number"
		? entry.max - entry.min : 0

	function write(value) {
		if (shell != null && entry != null) {
			shell.setPluginSetting(pluginId, entry.key, value)
		}
	}

	// A switch, a number and a text field each take one row; a choice takes
	// a row per choice, its own label above them.
	PanelSwitchRow {
		visible: root.entry != null && root.entry.type === "switch"
		label: root.entry != null ? root.entry.label : ""
		detail: root.entry != null ? root.entry.description : ""
		clickable: false
		checked: root.entry != null && root.entry.value === true
		onToggled: root.write(!(root.entry.value === true))
	}

	PanelRow {
		visible: root.entry != null && root.entry.type === "number"
		label: root.entry != null ? root.entry.label : ""
		detail: root.entry != null ? root.entry.description : ""
		clickable: false

		// The kit's slider runs 0..1: the declared range maps onto it, and
		// what comes back is rounded to the declared step.
		PanelSlider {
			width: 120
			value: root.span <= 0 ? 0
				: (Number(root.entry.value) - root.entry.min) / root.span
			onCommitted: function(fraction) {
				if (root.entry == null) {
					return
				}
				var step = root.entry.step > 0 ? root.entry.step : 1
				var raw = root.entry.min + fraction * root.span
				root.write(Math.round(raw / step) * step)
			}
		}
	}

	PanelRow {
		visible: root.entry != null && root.entry.type === "text"
		label: root.entry != null ? root.entry.label : ""
		detail: root.entry != null ? root.entry.description : ""
		clickable: false

		PanelField {
			width: 140
			text: root.entry != null && root.entry.value !== undefined
				? String(root.entry.value) : ""
			onAccepted: root.write(text)
		}
	}

	PanelText {
		visible: root.entry != null && root.entry.type === "choice"
		text: root.entry != null ? root.entry.label : ""
	}

	Repeater {
		model: root.entry != null && root.entry.type === "choice"
			? root.entry.choices : []

		ChoiceRow {
			required property var modelData

			label: modelData.label !== undefined ? modelData.label
				: String(modelData.value)
			description: modelData.description !== undefined
				? modelData.description : ""
			chosen: root.entry != null && root.entry.value === modelData.value
			onPicked: root.write(modelData.value)
		}
	}
}
