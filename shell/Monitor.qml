import Quickshell
import Quickshell.Io
import QtQuick

// Monitor: screen count as the glyph; the wheel steps backlight brightness
// by 5 % through brightnessctl. Hidden without a controllable backlight.
BarButton {
	id: root

	property int percent: 0
	property bool hasBacklight: false

	icon: true
	visible: hasBacklight
	text: Quickshell.screens.length > 1 ? "󰍺" : "󰍹"

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

	onWheeled: function(delta) {
		if (!hasBacklight) {
			return
		}
		// --min-value=1: a wheel step never blacks the screen out entirely.
		setBrightness.command = ["brightnessctl", "--class=backlight", "--min-value=1",
			"set", delta > 0 ? "5%+" : "5%-"]
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
