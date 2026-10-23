import QtQuick
import WaylandCanvas.Ui
import "."

// The Wi-Fi band of the connection in use, under its separator: the band
// the radio is on, with Automatic on the header line; pinned, the pills
// below offer the lock to a band the network is heard on.
Column {
	id: root

	readonly property var service: NetworkState.service

	spacing: UiTheme.panelSpacing

	PanelSeparator {}

	Item {
		width: parent.width
		height: UiTheme.sectionHeight

		PanelSectionHeader {
			id: bandHeader
			text: root.service.bandEffective === "auto" && root.service.bandCurrent !== ""
				? "Wi-Fi band: " + root.service.bandCurrent + " GHz" : "Wi-Fi band"
		}

		Row {
			anchors.right: parent.right
			anchors.verticalCenter: bandHeader.verticalCenter
			spacing: UiTheme.panelSpacing

			PanelSectionHeader {
				// inside the row: its own width, not the row's
				// (which would loop the layout)
				width: implicitWidth
				text: "Automatic"
			}

			PanelSwitch {
				checked: root.service.bandEffective === "auto"
				onToggled: root.service.toggleAuto()
			}
		}
	}

	Row {
		id: pills
		visible: root.service.bandEffective !== "auto"
			&& root.service.bandAvailable.length > 1
		width: parent.width
		spacing: UiTheme.panelSpacing

		Repeater {
			model: root.service.bandAvailable

			PanelPill {
				required property string modelData
				width: (parent.width - 2 * pills.spacing) / 3
				text: modelData + " GHz"
				active: root.service.bandCurrent === modelData
				chosen: root.service.bandEffective === modelData
				onClicked: root.service.setBand(modelData)
			}
		}
	}
}
