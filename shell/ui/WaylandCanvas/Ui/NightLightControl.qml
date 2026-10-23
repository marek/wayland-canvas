import QtQuick
import "."

// The night light's controls: a switch row naming the colour temperature and
// a slider for it, warm at the left (docs/intents/render/night-light.md). The
// values and the temperature's bounds come from `settings`, the answer of
// settings.get; it is idle until that arrives and reports choices by `chose`.
Column {
	id: root

	property var settings: null
	property string label: "Night light"
	// The settings.set param and the value the user chose for it.
	signal chose(string param, var value)

	readonly property var limits: settings && settings.fields
		? settings.fields.nightLightTemperature : null
	readonly property bool ready: limits != null && settings.session !== undefined
	readonly property bool on: ready && settings.session.nightLight === true
	readonly property int kelvin: ready ? settings.session.nightLightTemperature : 0
	readonly property real span: ready ? limits.max - limits.min : 1

	width: parent ? parent.width : 0
	spacing: UiTheme.panelSpacing
	enabled: ready

	PanelSwitchRow {
		label: root.label
		detail: root.ready ? root.kelvin + " K" : "—"
		clickable: false
		checked: root.on
		onToggled: root.chose("nightLight", !root.on)
	}

	PanelSlider {
		value: root.ready ? (root.kelvin - root.limits.min) / root.span : 0
		onMoved: function(v) {
			root.chose("nightLightTemperature", Math.round(root.limits.min + v * root.span))
		}
	}
}
