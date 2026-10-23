import Quickshell.Io
import Quickshell.Services.UPower
import QtQuick
import WaylandCanvas.Ui
import "."

// The power panel: the battery (level, state, time left), the power profile
// where the system offers one, and the session actions — the power menu
// lives here.
PanelCard {
	id: root

	panelWidth: 260

	readonly property var battery: PowerState.battery
	readonly property bool present: PowerState.present

	// Power profiles the system offers; balanced always, power saver with
	// it, performance only where the platform has one.
	readonly property var profiles: PowerProfiles.hasPerformanceProfile
		? [PowerProfile.PowerSaver, PowerProfile.Balanced, PowerProfile.Performance]
		: [PowerProfile.PowerSaver, PowerProfile.Balanced]

	// Seconds left, by direction: emptying on battery, filling while
	// charging. Firmware can report nonsense ranges (weeks): show nothing
	// rather than a lie.
	function timeLeft() {
		if (battery == null) {
			return ""
		}
		var charging = battery.state === UPowerDeviceState.Charging
			|| battery.state === UPowerDeviceState.PendingCharge
		var seconds = charging ? battery.timeToFull : battery.timeToEmpty
		if (seconds <= 0 || seconds > 72 * 3600) {
			return ""
		}
		var hours = Math.floor(seconds / 3600)
		var minutes = Math.floor((seconds % 3600) / 60)
		var clock = hours + ":" + (minutes < 10 ? "0" : "") + minutes
		return charging ? clock + " to full" : clock + " left"
	}

	function stateName() {
		if (battery == null) {
			return ""
		}
		switch (battery.state) {
		case UPowerDeviceState.Charging:
			return "Charging"
		case UPowerDeviceState.Discharging:
			return "On battery"
		case UPowerDeviceState.FullyCharged:
			return "Fully charged"
		case UPowerDeviceState.PendingCharge:
			return "Waiting to charge"
		case UPowerDeviceState.Empty:
			return "Empty"
		default:
			return "Unknown"
		}
	}

	function profileName(profile) {
		switch (profile) {
		case PowerProfile.PowerSaver:
			return "Power saver"
		case PowerProfile.Performance:
			return "Performance"
		default:
			return "Balanced"
		}
	}

	Process {
		id: systemctl
	}

	// The battery: everything the bar glyph says, spelled out.
	Item {
		width: parent.width
		height: 44

		Row {
			anchors.verticalCenter: parent.verticalCenter
			spacing: 10

			Text {
				anchors.verticalCenter: parent.verticalCenter
				text: root.present ? PowerState.glyph() : "󰁹"
				color: UiTheme.text
				font {
					family: UiTheme.fontFamily
					pixelSize: UiTheme.fontIcon
				}
				renderType: Text.NativeRendering
			}

			Column {
				spacing: 0
				anchors.verticalCenter: parent.verticalCenter

				PanelText {
					text: root.present ? PowerState.level + "%" : "No battery"
				}

				PanelText {
					dim: true
					text: root.stateName() + (root.timeLeft() !== "" ? " · " + root.timeLeft() : "")
				}
			}
		}
	}

	PanelText {
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "Power profile"
	}

	Repeater {
		model: root.profiles

		PanelRow {
			id: row
			required property var modelData
			readonly property var profile: modelData
			readonly property bool current: PowerProfiles.profile === profile
			glyph: current ? "󰄬" : ""
			label: root.profileName(profile)
			onClicked: PowerProfiles.profile = profile
		}
	}

	PanelText {
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "Session"
	}

	PanelRow {
		label: "Suspend"
		onClicked: {
			systemctl.command = ["systemctl", "suspend"]
			systemctl.running = true
		}
	}

	PanelRow {
		label: "Restart"
		onClicked: {
			systemctl.command = ["systemctl", "reboot"]
			systemctl.running = true
		}
	}

	PanelRow {
		label: "Power off"
		onClicked: {
			systemctl.command = ["systemctl", "poweroff"]
			systemctl.running = true
		}
	}

	PanelRow {
		label: "Log out"
		onClicked: api.compositor.call("quit")
	}
}
