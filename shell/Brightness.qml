import Quickshell
import Quickshell.Io
import QtQuick
import "."

// Brightness: the display glyph; the wheel steps the backlight by 5 %, the
// click opens the slider panel. Hidden without a controllable backlight.
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
		apply(percent + (delta > 0 ? 5 : -5))
	}

	onPressed: function(button) {
		if (button === Qt.LeftButton) {
			Panels.toggle(panel)
		}
	}

	BrightnessPanel {
		id: panel
		theme: root.theme
		anchorItem: root
		model: root
	}

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
