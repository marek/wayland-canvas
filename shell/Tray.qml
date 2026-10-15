import Quickshell
import Quickshell.Services.SystemTray
import Quickshell.Widgets
import QtQuick

// The system tray: one icon per status-notifier item; left activates, right
// secondary-activates. Hidden when the tray is empty.
Row {
	id: root

	property var theme: null
	spacing: 0
	visible: SystemTray.items.values.length > 0

	Repeater {
		model: SystemTray.items

		Item {
			id: item

			required property var modelData
			readonly property var entry: modelData

			width: root.visible ? (root.theme ? root.theme.iconSlot : 27) : 0
			height: root.theme ? root.theme.barHeight : 26

			IconImage {
				anchors.centerIn: parent
				source: item.entry.icon
				implicitSize: root.theme ? root.theme.iconSize : 16
			}

			MouseArea {
				anchors.fill: parent
				hoverEnabled: true
				cursorShape: Qt.PointingHandCursor
				acceptedButtons: Qt.LeftButton | Qt.RightButton
				onClicked: function(mouse) {
					if (mouse.button === Qt.RightButton) {
						item.entry.secondaryActivate()
					} else {
						item.entry.activate()
					}
				}
			}
		}
	}
}
