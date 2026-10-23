import QtQuick
import WaylandCanvas.Ui
import "nmcli.js" as Nm
import "."

// The DNS provider of the connection in use, under its separator: one pill
// for each provider of nmcli.js's list, and for the typed one a field for
// the servers. A pill applies its provider at once; the typed one opens the
// field, and Enter in it applies what was written.
Column {
	id: root

	readonly property var service: NetworkState.service
	readonly property var typedProvider: Nm.dnsProviders.find(function(provider) {
		return provider.typed
	})

	// Set from the panel's `open`: closing the panel folds the field away.
	property bool open: false
	property bool editing: false
	property string servers: ""

	spacing: UiTheme.panelSpacing

	onOpenChanged: if (!open) {
		editing = false
	}

	function pick(provider) {
		if (provider.typed) {
			editing = !editing
			return
		}
		service.setDns(provider.name, "")
	}

	function submit() {
		if (servers.trim() === "") {
			return
		}
		service.setDns(typedProvider.name, servers)
		editing = false
	}

	PanelSeparator {}

	PanelSectionHeader {
		text: "DNS provider"
	}

	Row {
		id: pills
		width: parent.width
		spacing: UiTheme.panelSpacing

		Repeater {
			model: Nm.dnsProviders

			PanelPill {
				required property var modelData
				readonly property bool typing: modelData.typed === true && root.editing
				width: (parent.width - (Nm.dnsProviders.length - 1) * pills.spacing)
					/ Nm.dnsProviders.length
				text: modelData.name
				active: root.service.dnsEffective === modelData.name && !typing
				chosen: typing
				onClicked: root.pick(modelData)
			}
		}
	}

	PanelField {
		id: dnsField
		visible: root.editing
		width: parent.width
		height: UiTheme.fieldHeight
		placeholderText: "DNS servers"
		onTextChanged: if (root.editing) root.servers = text
		onAccepted: root.submit()
		TapHandler { onTapped: dnsField.forceActiveFocus() }
		// the panel keeps the keys: Escape still closes after the
		// field folded away
		onVisibleChanged: if (!visible && root.open) {
			root.forceActiveFocus()
		}
	}
}
