import Quickshell
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui

// One bar surface: a top layer-shell panel with an exclusive zone, so maximized
// windows start below it. It follows the host's live widget list by section
// and order: a widget bar.hidden names is left out, and a hide or show adds or
// drops that widget alone, with no reload. Each widget carries its plugin's api
// and, when its manifest names one, the panel a left click opens.
PanelWindow {
	id: root

	property var api: null
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

	// The live widget items by name: a change of the list keeps the items of
	// the widgets that stay, with their state and an open panel's anchor.
	property var items: ({})

	function buildWidgets() {
		var rows = {left: leftSection, center: centreSection, right: rightSection}
		var shown = widgets.filter(function(widget) { return !widget.hidden })
		var kept = ({})
		for (var name in items) {
			if (shown.some(function(widget) { return widget.name === name })) {
				kept[name] = items[name]
			} else {
				items[name].parent = null // out of the row's list now, destroyed after
				items[name].destroy()
			}
		}
		var wanted = {left: [], center: [], right: []}
		for (var j = 0; j < shown.length; j++) {
			var widget = shown[j]
			if (kept[widget.name] === undefined) {
				var item = widget.component.createObject(rows[widget.section],
					{api: widget.api, panel: widget.panel})
				if (item === null) {
					continue
				}
				kept[widget.name] = item
			}
			wanted[widget.section].push(kept[widget.name])
		}
		items = kept
		for (var section in rows) {
			standInOrder(rows[section], wanted[section])
		}
	}

	// Makes the row's children stand in the wanted order. Items already in
	// place stay; from the first one out of place on, the rest are put
	// back in line, an item new to the row having been added last.
	function standInOrder(row, wanted) {
		for (var k = 0; k < wanted.length; k++) {
			if (row.children[k] !== wanted[k]) {
				for (var m = k; m < wanted.length; m++) {
					wanted[m].parent = null
				}
				for (m = k; m < wanted.length; m++) {
					wanted[m].parent = row
				}
				return
			}
		}
	}
}
