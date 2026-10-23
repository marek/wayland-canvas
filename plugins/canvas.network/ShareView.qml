import QtQuick
import WaylandCanvas.Ui
import "."

// The share view: the QR of the network in use, the payload fetched
// through NetworkManager's secrets and drawn module by module on a
// light card — dark on light with its quiet zone, the way scanners
// read it, and never as text: fonts put their own spacing between
// block characters and the code stops scanning.
Column {
	id: root

	readonly property var service: NetworkState.service
	readonly property var net: NetworkState.connectedNetwork

	spacing: UiTheme.panelSpacing

	PanelSectionHeader {
		text: root.net != null && root.net.name !== ""
			? "Share " + root.net.name : "Share network"
	}

	Rectangle {
		id: qrCard
		readonly property int size: root.service.qrRows.length
		readonly property int module: size > 0
			? Math.max(4, Math.floor(240 / size)) : 4
		anchors.horizontalCenter: parent.horizontalCenter
		width: size * module
		height: width
		radius: 4
		color: "#ffffff"

		Grid {
			anchors.centerIn: parent
			columns: qrCard.size

			Repeater {
				model: qrCard.size * qrCard.size

				Rectangle {
					required property int index
					width: qrCard.module
					height: qrCard.module
					color: root.service.qrRows[Math.floor(index / qrCard.size)]
						.charAt(index % qrCard.size) === "1"
						? "#111111" : "transparent"
				}
			}
		}
	}

	PanelRow {
		width: parent.width
		label: "Back"
		onClicked: root.service.closeShare()
	}
}
