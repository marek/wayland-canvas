import QtQuick
import WaylandCanvas.Ui
import "."

// The history panel: the silence switch, then what has arrived, newest
// first, and a way to forget all of it.
PanelCard {
	id: root

	panelWidth: 300
	panelName: "history"

	PanelRow {
		label: "Silence popups"
		detail: Notifications.silent
			? "Notifications are kept, not shown" : "Popups show as they arrive"
		onClicked: Notifications.setSilent(!Notifications.silent)

		PanelSwitch {
			anchors.right: parent.right
			anchors.verticalCenter: parent.verticalCenter
			checked: Notifications.silent
			onToggled: Notifications.setSilent(!Notifications.silent)
		}
	}

	PanelSeparator {}

	PanelText {
		visible: Notifications.history.length === 0
		text: "Nothing has arrived"
	}

	Repeater {
		model: Notifications.history

		delegate: PanelRow {
			required property var modelData

			label: modelData.summary
			detail: modelData.appName
				+ (modelData.body !== "" ? " · " + modelData.body : "")
			marked: modelData.urgency === "critical"
			clickable: false
		}
	}

	PanelSeparator {
		visible: Notifications.history.length > 0
	}

	PanelRow {
		visible: Notifications.history.length > 0
		label: "Clear the history"
		onClicked: Notifications.clearHistory()
	}
}
