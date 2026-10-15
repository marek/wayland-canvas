import Quickshell
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui

// One bar surface: a top layer-shell panel with an exclusive zone, so
// maximized windows start below it. The host hands the bar every loaded
// bar widget, by section and order (the config's bar.layout first, then
// each widget's declared order); each carries its own plugin's api and,
// when its manifest names one, the panel a left click opens.
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

	Component.onCompleted: {
		var widgets = registry != null ? registry.barWidgets : []
		for (var i = 0; i < widgets.length; i++) {
			var widget = widgets[i]
			var section = widget.section === "left" ? leftSection
				: widget.section === "center" ? centreSection : rightSection
			widget.component.createObject(section, {api: widget.api, panel: widget.panel})
		}
	}
}
