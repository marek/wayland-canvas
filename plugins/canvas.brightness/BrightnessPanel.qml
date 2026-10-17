import QtQuick
import WaylandCanvas.Ui

// The brightness panel: one slider for the backlight. The widget owns the
// brightnessctl plumbing; the panel only drives it through `apply`.
PanelCard {
	id: root

	panelWidth: 240
	panelName: "brightness"

	readonly property int percent: model ? model.percent : 0

	PanelText {
		width: parent.width
		height: UiTheme.sectionHeight
		text: "Brightness — " + root.percent + "%"
	}

	PanelSlider {
		value: root.percent / 100
		onMoved: function(v) {
			if (root.model != null) {
				root.model.apply(Math.round(v * 100))
			}
		}
	}

	// The night light lives beside the backlight: both are how bright and
	// how warm the screen is (docs/intents/render/night-light.md). The
	// compositor owns it; the panel only reads and writes its settings.
	PanelSeparator {}

	PanelRow {
		label: "Night light"
		detail: root.nightLight ? root.temperature + " K" : "Off"
		clickable: false

		PanelSwitch {
			anchors.right: parent.right
			anchors.rightMargin: 8
			anchors.verticalCenter: parent.verticalCenter
			checked: root.nightLight
			onToggled: root.setNightLight(!root.nightLight)
		}
	}

	PanelSlider {
		enabled: root.nightLight
		value: (root.temperature - 1000) / 5500
		onMoved: function(v) {
			root.setTemperature(Math.round(1000 + v * 5500))
		}
	}

	property bool nightLight: false
	property int temperature: 4000

	function readSession() {
		api.compositor.call("settings.get", undefined, function(result, error) {
			if (result == null || result.session === undefined) {
				return
			}
			nightLight = result.session.nightLight === true
			temperature = result.session.nightLightTemperature
		})
	}

	function setNightLight(on) {
		api.compositor.call("settings.set", {nightLight: on},
			function(result, error) { readSession() })
	}

	function setTemperature(kelvin) {
		api.compositor.call("settings.set", {nightLightTemperature: kelvin},
			function(result, error) { readSession() })
	}

	Component.onCompleted: readSession()

	Connections {
		target: api.compositor
		function onEvent(name, params) {
			if (name === "settings") {
				root.readSession()
			}
		}
	}
}
