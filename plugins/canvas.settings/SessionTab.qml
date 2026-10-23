import QtQuick
import WaylandCanvas.Ui

// The Session tab: the idle timeout before the screen locks, and the
// night light.
Column {
	id: root

	required property var model // the SettingsModel

	width: parent.width
	spacing: UiTheme.panelSpacing

	// canvas.conf's [session] values, as settings.get reports them.
	readonly property int lockIdleMinutes: model.settings && model.settings.session
		? model.settings.session.lockIdleMinutes : 5

	// The idle timeouts the page offers, in minutes; 0 is never.
	readonly property var idleChoices: [1, 5, 10, 30, 0]

	function idleLabel(minutes) {
		if (minutes === 0) {
			return "Never"
		}
		return minutes === 1 ? "After a minute" : "After " + minutes + " minutes"
	}

	PanelSectionHeader {
		text: "Lock the screen"
	}

	Repeater {
		model: root.idleChoices

		delegate: ChoiceRow {
			required property int modelData

			label: root.idleLabel(modelData)
			description: modelData === 0
				? "The screen never locks on its own" : "Of no input"
			chosen: root.lockIdleMinutes === modelData
			note: root.model.fieldFault("lockIdleMinutes")
			onPicked: root.model.setField("lockIdleMinutes", modelData)
		}
	}

	PanelSectionHeader {
		text: "Night light"
	}

	NightLightControl {
		label: "Warm the screen"
		settings: root.model.settings
		onChose: function(param, value) { root.model.setField(param, value) }
	}
}
