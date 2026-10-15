import Quickshell.Networking
import QtQuick

// The network panel: the Wi-Fi switch, the visible networks with signal and
// lock, and the connect flow for the selected one. Selecting a network never
// connects by itself — Connect (or Enter in the field) does — and the
// password field sits at the bottom of the panel, in a fixed place, so the
// list above never reflows it under a typing hand.
Panel {
	id: root

	panelWidth: 340
	panelName: "network"
	property var model: null // the network widget: device, wifi, networks

	property var selected: null
	property string password: ""

	readonly property var wifi: model ? model.wifi : null
	readonly property var device: model ? model.device : null
	readonly property bool wifiOn: Networking.wifiEnabled

	// Networks the system does not know are listed only while the device
	// scans: scan while the panel is open, never in the background.
	Binding {
		target: root.wifi
		property: "scannerEnabled"
		value: root.open
		when: root.wifi != null
	}

	// True when the selected network asks for a password we do not have.
	readonly property bool needsPassword: selected != null && !selected.connected
		&& !selected.known && secured(selected)

	function secured(network) {
		// OWE (enhanced open) encrypts without credentials: no prompt.
		return network.security !== WifiSecurityType.Open
			&& network.security !== WifiSecurityType.Owe
	}

	function select(network) {
		selected = network
		password = ""
	}

	function submit() {
		if (selected == null || selected.connected) {
			return
		}
		if (needsPassword) {
			if (password === "") {
				return
			}
			selected.connectWithPsk(password)
			password = ""
		} else {
			selected.connect()
		}
	}

	PanelRow {
		visible: root.wifi != null
		theme: root.theme
		label: "Wi-Fi"
		onClicked: Networking.wifiEnabled = !Networking.wifiEnabled

		PanelSwitch {
			anchors.right: parent.right
			anchors.rightMargin: 8
			anchors.verticalCenter: parent.verticalCenter
			theme: root.theme
			checked: root.wifiOn
			onToggled: Networking.wifiEnabled = !Networking.wifiEnabled
		}
	}

	// A wired link has nothing to configure: its state is the whole panel.
	PanelRow {
		visible: root.wifi == null && root.device != null
		theme: root.theme
		clickable: false
		glyph: root.device && root.device.type === DeviceType.Ethernet ? "󰈀" : "󰛳"
		label: root.device ? root.device.name : ""
		detail: root.device ? stateName(root.device) : ""

		function stateName(dev) {
			if (dev.connected) {
				return "Connected"
			}
			switch (dev.state) {
			case ConnectionState.Connecting:
				return "Connecting…"
			case ConnectionState.Disconnecting:
				return "Disconnecting…"
			case ConnectionState.Disconnected:
				return "Disconnected"
			default:
				return "Off"
			}
		}
	}

	PanelText {
		visible: root.wifi != null && !root.wifiOn
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "Wi-Fi is off"
	}

	PanelText {
		visible: root.wifi != null && root.wifiOn
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "Networks"
	}

	Repeater {
		model: root.wifi != null && root.wifiOn && root.wifi.networks
			? root.model.networks : []

		PanelRow {
			id: row
			required property var modelData
			readonly property var network: modelData
			theme: root.theme
			glyph: root.model ? root.model.wifiGlyph(root.model.percent(network)) : ""
			label: network.name !== "" ? network.name : "Hidden network"
			marked: network.connected
			onClicked: root.select(network)

			Text {
				anchors.right: parent.right
				anchors.rightMargin: 8
				anchors.verticalCenter: parent.verticalCenter
				text: (root.secured(row.network) ? "󰌾 " : "")
					+ (root.model ? root.model.percent(row.network) : 0) + "%"
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
		visible: root.wifi != null && root.wifiOn
			&& (!root.model || root.model.networks.length === 0)
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "None found"
	}

	// The selected network: what a row click armed, and the only path to a
	// connection or a disconnect.
	PanelText {
		visible: root.selected != null
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: root.selected
			? (root.selected.name !== "" ? root.selected.name : "Hidden network")
			: ""
	}

	PanelField {
		id: field
		visible: root.needsPassword
		width: parent.width
		height: 30
		theme: root.theme
		password: true
		placeholderText: "Password"
		onTextChanged: root.password = text
		onAccepted: root.submit()
	}

	PanelRow {
		visible: root.selected != null
		theme: root.theme
		label: root.selected && root.selected.connected ? "Disconnect" : "Connect"
		marked: root.selected && root.selected.connected
		onClicked: {
			if (root.selected == null) {
				return
			}
			if (root.selected.connected) {
				root.selected.disconnect()
			} else {
				root.submit()
			}
		}
	}
}
