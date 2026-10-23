import Quickshell.Bluetooth
import QtQuick
import WaylandCanvas.Ui
import "."

// The Bluetooth panel: radio power, scanning, and the devices — paired ones
// first, then what a scan turned up. A device row's click is its natural
// action: connect or disconnect a paired device, pair a found one.
PanelCard {
	id: root

	panelWidth: 300

	readonly property var adapter: BluetoothState.adapter
	readonly property bool radioOn: BluetoothState.radioOn

	// A device the system keeps: paired (with its keys stored, or not) or
	// trusted. BlueZ sets Bonded only when the pairing keys are stored.
	function known(device) {
		return device.paired || device.bonded || device.trusted
	}

	// Known devices ahead of mere findings, each half alphabetical.
	readonly property var devices: {
		var list = adapter && adapter.devices ? adapter.devices.values.slice() : []
		list.sort(function(a, b) {
			if (known(a) !== known(b)) {
				return known(a) ? -1 : 1
			}
			return a.deviceName.localeCompare(b.deviceName)
		})
		return list
	}

	function actOn(device) {
		if (known(device)) {
			if (device.connected) {
				device.disconnect()
			} else {
				device.connect()
			}
		} else {
			device.pair()
		}
	}

	function stateName(device) {
		if (device.connected) {
			return "Connected"
		}
		if (device.pairing) {
			return "Pairing…"
		}
		if (known(device)) {
			return "Paired"
		}
		return "Found"
	}

	PanelSwitchRow {
		visible: root.adapter != null
		label: "Bluetooth"
		checked: root.radioOn
		onToggled: if (root.adapter != null) root.adapter.enabled = !root.adapter.enabled
	}

	PanelSwitchRow {
		visible: root.radioOn
		label: "Scan"
		checked: root.adapter != null && root.adapter.discovering
		onToggled: if (root.adapter != null) root.adapter.discovering = !root.adapter.discovering
	}

	PanelSectionHeader {
		visible: root.radioOn
		text: "Devices"
	}

	Repeater {
		model: root.radioOn ? root.devices : []

		PanelRow {
			id: row
			required property var modelData
			readonly property var device: modelData
			label: device.deviceName !== "" ? device.deviceName : device.address
			marked: device.connected
			onClicked: root.actOn(device)

			PanelText {
				dim: true
				text: root.stateName(row.device)
			}
		}
	}

	PanelText {
		visible: root.radioOn && root.devices.length === 0
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "None found"
	}

	PanelText {
		visible: root.adapter != null && !root.radioOn
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "Bluetooth is off"
	}
}
