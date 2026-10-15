import Quickshell
import Quickshell.Io
import QtQuick

// The power menu, anchored under the power button: suspend, restart, power
// off, or end the session through the control socket. Closes on an action,
// on the button again, or when the pointer leaves it.
PopupWindow {
	id: root

	property var theme: null
	property var anchorItem: null

	visible: false
	implicitWidth: 200
	implicitHeight: column.implicitHeight + 16
	color: theme ? theme.background : "#1a1b26"

	anchor.window: root.anchorItem ? root.anchorItem.QsWindow.window : null
	anchor.edges: Edges.Bottom | Edges.Left
	anchor.gravity: Edges.Bottom | Edges.Right
	anchor.adjustment: PopupAdjustment.Slide
	anchor.rect.x: root.anchorItem
		? root.anchorItem.mapToItem(null, 0, 0).x : 0
	anchor.rect.y: root.anchorItem
		? root.anchorItem.mapToItem(null, 0, root.anchorItem.height).y : 0
	anchor.rect.width: 1
	anchor.rect.height: 1

	Ipc {
		id: ipc
	}

	Process {
		id: systemctl
	}

	Column {
		id: column
		anchors.fill: parent
		anchors.margins: 8
		spacing: 2

		MenuRow {
			label: "Suspend"
			onChosen: function() {
				systemctl.command = ["systemctl", "suspend"]
				systemctl.running = true
			}
		}
		MenuRow {
			label: "Restart"
			onChosen: function() {
				systemctl.command = ["systemctl", "reboot"]
				systemctl.running = true
			}
		}
		MenuRow {
			label: "Power off"
			onChosen: function() {
				systemctl.command = ["systemctl", "poweroff"]
				systemctl.running = true
			}
		}
		MenuRow {
			label: "Log out"
			onChosen: ipc.send("quit")
		}
	}

	HoverHandler {
		onHoveredChanged: if (!hovered) leaveTimer.restart()
	}

	Timer {
		id: leaveTimer
		interval: 800
		onTriggered: root.visible = false
	}

	component MenuRow: Item {
		id: row

		property string label: ""
		signal chosen()

		implicitWidth: parent.width
		implicitHeight: 30

		Rectangle {
			anchors.fill: parent
			radius: 4
			color: rowMouse.containsMouse
				? Qt.rgba(1, 1, 1, 0.08)
				: "transparent"
		}

		Text {
			anchors.centerIn: parent
			text: row.label
			color: root.theme ? root.theme.text : "#a9b1d6"
			font {
				family: root.theme ? root.theme.fontFamily : "monospace"
				pixelSize: root.theme ? root.theme.fontBody : 12
			}
		}

		MouseArea {
			id: rowMouse
			anchors.fill: parent
			hoverEnabled: true
			cursorShape: Qt.PointingHandCursor
			onClicked: function() {
				row.chosen()
				root.visible = false
			}
		}
	}
}
