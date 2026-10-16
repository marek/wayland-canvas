import Quickshell.Io
import QtQuick
import WaylandCanvas.Ui

// The agents widget: the robot glyph while any AI provider has usage to
// show, none while the machine is quiet. It runs the collector helper by
// name — the manifest's refresh interval, plus every panel open — and
// carries the attention colour the moment any limits window is nearly
// spent, so a burning session is visible without opening the panel.
BarButton {
	id: root

	property var providers: []

	readonly property int refreshMs: (api && api.settings
		&& api.settings.refreshIntervalSec || 900) * 1000

	text: providers.length > 0 ? "󱚣" : ""
	active: anyLimitSpent()

	function refresh() {
		if (!collect.running) {
			collect.running = true
		}
	}

	Process {
		id: collect
		command: ["wayland-canvas-agent-usage"]
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: root.parse(text)
		}
	}

	Timer {
		interval: root.refreshMs
		repeat: true
		running: true
		triggeredOnStart: true
		onTriggered: root.refresh()
	}

	onPressed: function(button) {
		if (button !== Qt.LeftButton) {
			refresh()
		}
	}

	function parse(raw) {
		try {
			providers = JSON.parse(raw).providers || []
		} catch (e) {
			// A failed run keeps the last good record.
		}
	}

	function anyLimitSpent() {
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
