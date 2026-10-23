pragma Singleton

import Quickshell.Io
import QtQuick

// The AI providers' usage, which the agents widget on every bar and the
// agents panel read: one collector run feeds them all. The helper runs by
// name, at the manifest's refresh interval while a widget is up, and again
// whenever the panel opens.
Item {
	id: root

	property var providers: []
	property var api: null

	// The widgets that are up; the timer runs while any is.
	property int watchers: 0

	readonly property int refreshMs: (api !== null
		? api.plugin.settings.refreshIntervalSec : 900) * 1000

	// The plugin's api, for its refresh setting; the first caller's stays.
	function init(pluginApi) {
		if (api === null) {
			api = pluginApi
		}
	}

	function watch() {
		watchers++
	}

	function unwatch() {
		watchers--
	}

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
		running: root.watchers > 0
		triggeredOnStart: true
		onTriggered: root.refresh()
	}

	function parse(raw) {
		try {
			providers = JSON.parse(raw).providers || []
		} catch (e) {
			// A failed run keeps the last good record.
			console.warn("[agents] parse failed: " + e)
		}
	}
}
