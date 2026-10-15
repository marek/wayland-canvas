import Quickshell
import Quickshell.Services.SystemTray
import Quickshell.Widgets
import QtQuick
import WaylandCanvas.Ui
import "."

// The system tray: one icon per status-notifier item; left activates, right
// opens the item's own menu (the plugin's panel, through the common
// trigger). Hidden when the tray is empty.
Row {
	id: root

	property var api: null
	property string panel: ""
	spacing: 0
	visible: SystemTray.items.values.length > 0

	Repeater {
		model: SystemTray.items

		Item {
			id: item

			required property var modelData
			readonly property var entry: modelData

			width: root.visible ? UiTheme.iconSlot : 0
			height: UiTheme.barHeight

			IconImage {
				anchors.centerIn: parent
				source: item.entry.icon
				implicitSize: UiTheme.iconSize
			}

			MouseArea {
				anchors.fill: parent
				hoverEnabled: true
				cursorShape: Qt.PointingHandCursor
				acceptedButtons: Qt.LeftButton | Qt.RightButton
				onClicked: function(mouse) {
					if (mouse.button === Qt.RightButton) {
						if (item.entry.hasMenu) {
							// The menu panel is opened through the common
							// trigger, anchored to this icon; the item it
							// shows lives in TrayState.
							TrayState.activeItem = item.entry
							if (root.api != null) {
								root.api.panels.toggle("canvas.tray/tray", item)
							}
						} else {
							item.entry.secondaryActivate()
						}
					} else {
						item.entry.activate()
					}
				}
			}
		}
	}
}
