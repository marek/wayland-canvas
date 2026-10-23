import QtQuick
import WaylandCanvas.Ui
import "."

// The agents widget: the robot glyph while any AI provider has usage to
// show, none while the machine is quiet. AgentsState runs the collector
// helper — the manifest's refresh interval, plus every panel open; the
// widget carries the attention colour the moment any limits window is
// nearly spent, so a burning session is visible without opening the panel.
BarButton {
	id: root

	text: AgentsState.providers.length > 0 ? "󱚣" : ""
	active: anyLimitSpent()

	Component.onCompleted: {
		AgentsState.init(api)
		AgentsState.watch()
	}

	Component.onDestruction: AgentsState.unwatch()

	onPressed: function(button) {
		if (button !== Qt.LeftButton) {
			AgentsState.refresh()
		}
	}

	function anyLimitSpent() {
		var providers = AgentsState.providers
		for (var i = 0; i < providers.length; i++) {
			var limits = providers[i].limits || []
			for (var j = 0; j < limits.length; j++) {
				if (limits[j].percent >= 0.9) {
					return true
				}
			}
		}
		return false
	}
}
