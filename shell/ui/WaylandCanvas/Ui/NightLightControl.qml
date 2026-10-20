import QtQuick
import "."

// The night light's controls: a switch row that names the colour
// temperature, and a slider for it, warm at the left. The compositor owns
// the night light (docs/intents/render/night-light.md): the control reads the
// switch, the temperature and the temperature's bounds from `settings`, the
// answer of settings.get, and reports what the user chose through `chose`,
// which the caller hands to settings.set. It knows no bound of its own, so
// until the answer arrives it is idle.
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

	PanelRow {
		label: root.label
		detail: root.ready ? root.kelvin + " K" : "—"
		clickable: false

		PanelSwitch {
			anchors.right: parent.right
			anchors.rightMargin: 8
			anchors.verticalCenter: parent.verticalCenter
			checked: root.on
			onToggled: root.chose("nightLight", !root.on)
		}
	}

	PanelSlider {
		value: root.ready ? (root.kelvin - root.limits.min) / root.span : 0
		onMoved: function(v) {
			root.chose("nightLightTemperature", Math.round(root.limits.min + v * root.span))
		}
	}
}
