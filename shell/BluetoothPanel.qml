import Quickshell.Bluetooth
import QtQuick

// The Bluetooth panel: radio power, scanning, and the devices — paired ones
// first, then what a scan turned up. A device row's click is its natural
// action: connect or disconnect a paired device, pair a found one.
Panel {
	id: root

	panelWidth: 300
	panelName: "bluetooth"
	property var model: null // the bluetooth widget: adapter

	readonly property var adapter: model ? model.adapter : null
	readonly property bool radioOn: adapter != null && adapter.enabled

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

	PanelRow {
		visible: root.adapter != null
		theme: root.theme
		label: "Bluetooth"
		onClicked: if (root.adapter != null) root.adapter.enabled = !root.adapter.enabled

		PanelSwitch {
			anchors.right: parent.right
			anchors.rightMargin: 8
			anchors.verticalCenter: parent.verticalCenter
			theme: root.theme
			checked: root.radioOn
			onToggled: if (root.adapter != null)
				root.adapter.enabled = !root.adapter.enabled
		}
	}

	PanelRow {
		visible: root.radioOn
		theme: root.theme
		label: "Scan"
		onClicked: if (root.adapter != null) root.adapter.discovering = !root.adapter.discovering

		PanelSwitch {
			anchors.right: parent.right
			anchors.rightMargin: 8
			anchors.verticalCenter: parent.verticalCenter
			theme: root.theme
			checked: root.adapter != null && root.adapter.discovering
			onToggled: if (root.adapter != null)
				root.adapter.discovering = !root.adapter.discovering
		}
	}

	PanelText {
		visible: root.radioOn
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "Devices"
	}

	Repeater {
		model: root.radioOn ? root.devices : []

		PanelRow {
			id: row
			required property var modelData
			readonly property var device: modelData
			theme: root.theme
			label: device.deviceName !== "" ? device.deviceName : device.address
			marked: device.connected
			onClicked: root.actOn(device)

			Text {
				anchors.right: parent.right
				anchors.rightMargin: 8
				anchors.verticalCenter: parent.verticalCenter
				text: root.stateName(row.device)
				color: root.theme ? root.theme.muted : "#565f89"
				font {
					family: root.theme ? root.theme.fontFamily : "monospace"
					pixelSize: root.theme ? root.theme.fontBody : 12
				}
				renderType: Text.NativeRendering
			}
		}
	}

	PanelText {
		visible: root.radioOn && root.devices.length === 0
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "None found"
	}

	PanelText {
		visible: root.adapter != null && !root.radioOn
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "Bluetooth is off"
	}
}
