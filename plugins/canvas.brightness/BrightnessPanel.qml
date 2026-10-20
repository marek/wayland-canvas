import QtQuick
import WaylandCanvas.Ui

// The brightness panel: one slider for the backlight. The widget owns the
// brightnessctl plumbing; the panel only drives it through `apply`.
PanelCard {
	id: root

	panelWidth: 240

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

	NightLightControl {
		settings: root.settings
		onChose: function(param, value) { root.setSetting(param, value) }
	}

	// The settings.get answer the night light's control reads, null until
	// it arrives.
	property var settings: null

	function readSettings() {
		api.compositor.call("settings.get", undefined, function(result, error) {
			if (result != null) {
				settings = result
			}
		})
	}

	function setSetting(param, value) {
		var params = {}
		params[param] = value
		api.compositor.call("settings.set", params,
			function(result, error) { readSettings() })
	}

	Component.onCompleted: readSettings()

	Connections {
		target: api.compositor
		function onEvent(name, params) {
			if (name === "settings") {
				root.readSettings()
			}
		}
	}
}
