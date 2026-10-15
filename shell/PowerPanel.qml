import Quickshell.Io
import Quickshell.Services.UPower
import QtQuick

// The power panel: the battery (level, state, time left), the power profile
// where the system offers one, and the session actions — the power menu
// lives here.
Panel {
	id: root

	panelWidth: 260
	panelName: "power"
	property var model: null // the power widget: battery, glyph

	readonly property var battery: model ? model.battery : null
	readonly property bool present: battery != null && battery.isPresent

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

	Ipc {
		id: ipc
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
				text: root.model && root.present ? root.model.glyph() : "󰁹"
				color: root.theme ? root.theme.text : "#a9b1d6"
				font {
					family: root.theme ? root.theme.fontFamily : "monospace"
					pixelSize: root.theme ? root.theme.fontIcon : 13
				}
				renderType: Text.NativeRendering
			}

			Column {
				spacing: 0
				anchors.verticalCenter: parent.verticalCenter

				PanelText {
					theme: root.theme
					text: root.present ? root.model.level + "%" : "No battery"
				}

				PanelText {
					theme: root.theme
					dim: true
					text: root.stateName() + (root.timeLeft() !== "" ? " · " + root.timeLeft() : "")
				}
			}
		}
	}

	PanelText {
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "Power profile"
	}

	Repeater {
		model: root.profiles

		PanelRow {
			id: row
			required property var modelData
			readonly property var profile: modelData
			readonly property bool current: PowerProfiles.profile === profile
			theme: root.theme
			glyph: current ? "󰄬" : ""
			label: root.profileName(profile)
			onClicked: PowerProfiles.profile = profile
		}
	}

	PanelText {
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "Session"
	}

	PanelRow {
		theme: root.theme
		label: "Suspend"
		onClicked: {
			systemctl.command = ["systemctl", "suspend"]
			systemctl.running = true
		}
	}

	PanelRow {
		theme: root.theme
		label: "Restart"
		onClicked: {
			systemctl.command = ["systemctl", "reboot"]
			systemctl.running = true
		}
	}

	PanelRow {
		theme: root.theme
		label: "Power off"
		onClicked: {
			systemctl.command = ["systemctl", "poweroff"]
			systemctl.running = true
		}
	}

	PanelRow {
		theme: root.theme
		label: "Log out"
		onClicked: ipc.send("quit")
	}
}
