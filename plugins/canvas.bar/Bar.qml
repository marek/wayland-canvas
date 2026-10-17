import Quickshell
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui

// One bar surface: a top layer-shell panel with an exclusive zone, so
// maximized windows start below it. The bar reads every loaded widget
// from the host's live list, by section and order (the config's
// bar.layout first, then each widget's declared order); a widget the
// config's bar.hidden names is flagged there and left out here, and
// because the list is live the settings panel hides or shows a widget
// without reloading the shell — this bar rebuilds its rows when the
// list changes. Each widget carries its own plugin's api and, when its
// manifest names one, the panel a left click opens.
PanelWindow {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null // the output this bar is on

	screen: modelData
	anchors {
		top: true
		left: true
		right: true
	}
	exclusionMode: ExclusionMode.Auto
	implicitHeight: UiTheme.barHeight
	color: UiTheme.background
	WlrLayershell.namespace: "wayland-canvas-bar"
	WlrLayershell.layer: WlrLayer.Top

	readonly property var widgets: api != null && api.shell != null
		? api.shell.barWidgets : []

	onWidgetsChanged: buildWidgets()
	Component.onCompleted: buildWidgets()

	Item {
		anchors.fill: parent

		Row {
			id: leftSection
			spacing: 0
			anchors.left: parent.left
			anchors.leftMargin: UiTheme.edgeMargin
			anchors.verticalCenter: parent.verticalCenter
		}

		Row {
			id: centreSection
			spacing: 0
			anchors.centerIn: parent
		}

		Row {
			id: rightSection
			spacing: 0
			anchors.right: parent.right
			anchors.rightMargin: UiTheme.edgeMargin
			anchors.verticalCenter: parent.verticalCenter
		}
	}

	function buildWidgets() {
		var rows = {left: leftSection, center: centreSection, right: rightSection}
		for (var section in rows) {
			var children = rows[section].children
			for (var i = children.length - 1; i >= 0; i--) {
				children[i].destroy()
			}
		}
		for (var j = 0; j < widgets.length; j++) {
			var widget = widgets[j]
			if (widget.hidden) {
				continue
			}
			widget.component.createObject(rows[widget.section],
				{api: widget.api, panel: widget.panel})
		}
	}
}
