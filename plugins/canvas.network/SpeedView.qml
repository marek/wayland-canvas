import QtQuick
import WaylandCanvas.Ui
import "format.js" as Format
import "."

// The speed-test view: the tool's answer as rows, until Back.
Column {
	id: root

	readonly property var service: NetworkState.service
	readonly property var result: service.speedResult

	spacing: UiTheme.panelSpacing

	PanelSectionHeader {
		text: "Speed test"
	}

	PanelRow {
		width: parent.width
		clickable: false
		label: "Download"
		detail: root.result != null ? Format.formatSpeed(root.result.down) : ""
	}

	PanelRow {
		width: parent.width
		clickable: false
		label: "Upload"
		detail: root.result != null ? Format.formatSpeed(root.result.up) : ""
	}

	PanelRow {
		width: parent.width
		clickable: false
		label: "Ping"
		detail: root.result != null && isFinite(root.result.ping)
			? Format.formatMs(root.result.ping) : ""
	}

	PanelRow {
		width: parent.width
		label: "Back"
		onClicked: root.service.closeSpeed()
	}
}
