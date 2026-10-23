import QtQuick
import WaylandCanvas.Ui

// One app of the app bar: its desktop entry's icon in a square — the app
// id's first letter where the entry names no icon — with a dot per window up
// to two, dimmed while all its windows are minimized and accented while it
// holds the focus. Hovering names the app on the chrome surface; a click
// goes to its window, or unfolds its windows when it has several
// (docs/intents/shell/app-bar.md).
Item {
	id: root

	property var app: null // the grouped entry: key, appId, name, icon, windows
	property var surface: null // the app bar surface, which acts on clicks

	// The strip lays the entry out; the square is its.
	width: surface != null ? surface.squareSize : 0
	height: width

	readonly property string mark: app != null && app.name !== ""
		? app.name.substring(0, 1).toUpperCase() : "?"
	readonly property bool dim: app != null && app.minimized
	readonly property bool open: surface != null && app != null
		&& surface.flyoutKey === app.key

	Rectangle {
		anchors.fill: parent
		radius: UiTheme.radius
		visible: mouse.containsMouse || root.open
		color: root.open ? UiTheme.selected : UiTheme.hover
	}

	// The icon the desktop entry names; the letter stands in for an app
	// without one (or whose icon the theme does not hold).
	Image {
		anchors.centerIn: parent
		width: 24
		height: 24
		sourceSize.width: 24
		sourceSize.height: 24
		fillMode: Image.PreserveAspectFit
		smooth: true
		opacity: root.dim ? 0.55 : 1
		visible: status === Image.Ready
		source: root.app != null && root.app.icon !== "" ? root.app.icon : ""
	}

	Text {
		anchors.centerIn: parent
		visible: root.app == null || root.app.icon === ""
		text: root.mark
		color: root.app != null && root.app.focused
			? UiTheme.accent : (root.dim ? UiTheme.muted : UiTheme.text)
		font {
			family: UiTheme.fontFamily
			pixelSize: UiTheme.fontIcon
			bold: root.app != null && root.app.focused
		}
		renderType: Text.NativeRendering
	}

	// One dot, or two for an app with more than one window; the accent
	// marks the app holding the focus.
	Row {
		anchors.horizontalCenter: parent.horizontalCenter
		anchors.bottom: parent.bottom
		anchors.bottomMargin: 4
		spacing: 3

		Repeater {
			model: root.app != null && root.app.windows.length > 1 ? 2 : 1

			Rectangle {
				width: 3
				height: 3
				radius: 1.5
				color: root.app != null && root.app.focused ? UiTheme.accent
					: (root.dim ? UiTheme.muted : UiTheme.text)
			}
		}
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		onEntered: if (root.surface != null && root.app != null) {
			root.surface.hoveredKey = root.app.key
		}
		onExited: if (root.surface != null && root.app != null
				&& root.surface.hoveredKey === root.app.key) {
			root.surface.hoveredKey = ""
		}
		onClicked: if (root.surface != null && root.app != null) {
			root.surface.appActivated(root.app)
		}
	}
}
