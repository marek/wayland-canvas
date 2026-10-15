import Quickshell
import Quickshell.Wayland
import QtQuick

// One bar surface: a top layer-shell panel with an exclusive zone, so
// maximized windows start below it. Sections: logo left, clock and weather
// centred, status widgets right.
PanelWindow {
	id: root

	required property var modelData

	screen: modelData
	anchors {
		top: true
		left: true
		right: true
	}
	exclusionMode: ExclusionMode.Auto
	implicitHeight: theme.barHeight
	color: theme.background
	WlrLayershell.namespace: "wayland-canvas-bar"
	WlrLayershell.layer: WlrLayer.Top

	Theme {
		id: theme
	}

	Item {
		anchors.fill: parent

		Logo {
			theme: theme
			anchors.left: parent.left
			anchors.leftMargin: theme.edgeMargin
			anchors.verticalCenter: parent.verticalCenter
		}

		Row {
			spacing: 0
			anchors.centerIn: parent

			Clock {
				theme: theme
			}
			Weather {
				theme: theme
			}
		}

		Row {
			spacing: 0
			anchors.right: parent.right
			anchors.rightMargin: theme.edgeMargin
			anchors.verticalCenter: parent.verticalCenter

			Tray {
				theme: theme
			}
			Bluetooth {
				theme: theme
			}
			Network {
				theme: theme
			}
			Audio {
				theme: theme
			}
			Brightness {
				theme: theme
			}
			Power {
				theme: theme
			}
		}
	}
}
