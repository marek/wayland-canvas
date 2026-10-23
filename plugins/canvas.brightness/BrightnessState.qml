pragma Singleton

import Quickshell.Io
import QtQuick

// The backlight, as the brightness widget on every bar and the slider panel
// share it: one value however many bars there are, and the brightnessctl
// plumbing that reads and sets it.
Item {
	id: root

	property int percent: 0
	property bool hasBacklight: false

	Process {
		id: readBrightness
		command: ["brightnessctl", "--class=backlight", "-m", "info"]
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: root.parse(text)
		}
		onExited: function(exitCode) {
			root.hasBacklight = exitCode === 0
		}
	}

	Process {
		id: setBrightness
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: root.parse(text)
		}
	}

	Component.onCompleted: readBrightness.running = true

	// A step from the wheel or the panel slider. --min-value=1: the screen
	// never blacks out entirely.
	function apply(pct) {
		if (!hasBacklight) {
			return
		}
		pct = Math.max(1, Math.min(100, Math.round(pct)))
		setBrightness.command = ["brightnessctl", "--class=backlight", "--min-value=1",
			"set", pct + "%"]
		setBrightness.running = true
	}

	// brightnessctl -m prints one line per backlight: name,type,cur,max,PCT%.
	function parse(raw) {
		var first = String(raw || "").split("\n")[0]
		var match = first.match(/(\d+)%/)
		if (match) {
			percent = Number(match[1])
			hasBacklight = true
		}
	}
}
