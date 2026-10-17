import QtQuick
import WaylandCanvas.Ui

// One app of the app bar: its mark in a square, a click routed to the
// surface, which goes to that app's window. An app with more than one window
// carries a second dot; an app whose windows are all minimized is dimmed;
// the app holding the focus is marked with the accent.
Item {
	id: root

	property var app: null // the grouped entry: appId, name, windows
	property var surface: null // the app bar surface, which acts on clicks

	// The mark: the app id's first letter, as the bar draws glyphs rather
	// than images.
	readonly property string mark: app != null && app.name !== ""
		? app.name.substring(0, 1).toUpperCase() : "?"

	Rectangle {
		anchors.fill: parent
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	Text {
		anchors.centerIn: parent
		text: root.mark
		color: root.app != null && root.app.focused
			? UiTheme.accent
			: (root.app != null && root.app.minimized ? UiTheme.muted : UiTheme.text)
		font {
			family: UiTheme.fontFamily
			pixelSize: UiTheme.fontIcon
			bold: root.app != null && root.app.focused
		}
		renderType: Text.NativeRendering
	}

	// How many windows the app holds: one dot, or two for more than one.
	Row {
		anchors.horizontalCenter: parent.horizontalCenter
		anchors.bottom: parent.bottom
		anchors.bottomMargin: 5
		spacing: 3

		Repeater {
			model: root.app != null && root.app.windows.length > 1 ? 2 : 1

			Rectangle {
				width: 3
				height: 3
				radius: 1.5
				color: root.app != null && root.app.minimized
					? UiTheme.muted : UiTheme.text
			}
		}
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		onEntered: if (root.surface != null) root.surface.hoveredApp = root.app
		onExited: if (root.surface != null && root.surface.hoveredApp === root.app) {
			root.surface.hoveredApp = null
		}
		onClicked: if (root.surface != null) root.surface.appActivated(root.app)
	}
}
