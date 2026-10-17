import Quickshell
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui
import "."

// One app bar: while its screen shows the overview, a strip of every open
// app slides in on the edge the shell config's appbar.position names — the
// left by default — and slides out as the screen leaves
// (docs/intents/shell/app-bar.md). It launches nothing: a click goes to that
// app's window, which the compositor focuses and moves the view to
// (window.focus, docs/intents/platform/ipc.md).
//
// On the toolbar's own edge the two read as one strip: the toolbar's buttons
// first, the separator in the gap, the apps last. Neither surface knows the
// other — both place themselves from WaylandCanvas.Ui's StripLayout with the
// two lengths, which each reads off the shell config, the toolbar registry
// and the compositor's window list.
Item {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null // the output this app bar is on

	readonly property int entrySize: 44
	readonly property int slideMs: 200
	readonly property int edgeMargin: UiTheme.edgeMargin

	// The host item, which holds the live config (as the toolbar reads it).
	readonly property var shell: {
		if (api == null) {
			return null
		}
		if (api.shell !== undefined) {
			return api.shell
		}
		return api.plugin != null ? api.plugin.host : null
	}

	readonly property var config: shell != null ? shell.config : null

	// appbar.enabled: off leaves no surface on any screen.
	readonly property bool switchedOn: {
		var section = config != null ? config.appbar : null
		return !(section != null && section.enabled === false)
	}

	// The edge appbar.position names, the left when it names none or
	// something else: a hand edit can hold anything.
	readonly property string edge: {
		var section = config != null ? config.appbar : null
		var named = section != null && section.position !== undefined
			? section.position : ""
		return named === "right" || named === "top" || named === "bottom"
			? named : "left"
	}
	readonly property bool vertical: edge === "left" || edge === "right"

	// The toolbar's edge, and whether it is this one: the strips then share
	// it, the apps after the toolbar's buttons.
	readonly property string toolbarEdge: {
		var section = config != null ? config.toolbar : null
		var named = section != null && section.position !== undefined
			? section.position : ""
		return named === "left" || named === "top" || named === "bottom"
			? named : "right"
	}
	readonly property bool shared: toolbarEdge === edge

	// ---- the apps ----

	// One entry per app, in the order the apps' first windows opened: the
	// window list is in that order already. Each entry keeps its app's
	// windows, newest-focused first, so a click walks them.
	readonly property var apps: groupWindows()

	function groupWindows() {
		var windows = api != null && api.compositor != null
			? (api.compositor.state.windows || []) : []
		var byApp = ({})
		var out = []
		for (var i = 0; i < windows.length; i++) {
			var w = windows[i]
			var key = w.appId !== "" ? w.appId : ("window:" + w.id)
			if (byApp[key] === undefined) {
				byApp[key] = {
					appId: w.appId,
					name: w.appId !== "" ? w.appId : w.title,
					windows: [],
					minimized: true,
					focused: false,
				}
				out.push(byApp[key])
			}
			var app = byApp[key]
			app.windows.push(w)
			if (!w.minimized) {
				app.minimized = false
			}
			if (w.focused) {
				app.focused = true
			}
		}
		return out
	}

	// Whether this screen shows the overview, from the compositor's state.
	readonly property bool overview: {
		var outputs = api != null && api.compositor != null
			? api.compositor.state.outputs : []
		for (var i = 0; i < outputs.length; i++) {
			if (outputs[i].name === modelData.name) {
				return outputs[i].overview === true
			}
		}
		return false
	}

	// ---- the strip's place ----

	readonly property int stripLength: apps.length * entrySize
	// The toolbar's own length on this edge, for the shared strip: the
	// buttons it would draw, from the same registry the toolbar reads.
	readonly property int toolbarLength: {
		if (!shared || registry == null) {
			return 0
		}
		var all = registry.toolbarButtons || []
		var count = 0
		for (var i = 0; i < all.length; i++) {
			var button = all[i]
			var kinds = (button.action !== "" ? 1 : 0)
				+ (button.panel !== "" ? 1 : 0)
				+ (button.menu.length > 0 ? 1 : 0)
			if (kinds === 1) {
				count++
			}
		}
		return count * entrySize
	}

	readonly property int nearEdge: edge === "top"
		? UiTheme.barHeight + edgeMargin : edgeMargin
	readonly property int pocket: nearEdge + entrySize
	property real slide: overview ? 0 : -pocket
	Behavior on slide {
		NumberAnimation {
			id: slideAnim
			duration: root.slideMs
			easing.type: Easing.OutCubic
		}
	}

	// Along its edge: after the toolbar's part when they share the edge,
	// else the strip centred on its own.
	readonly property int along: StripLayout.appsStart(
		vertical ? modelData.height : modelData.width, toolbarLength, stripLength)

	// ---- the strip ----

	PanelWindow {
		id: strip

		screen: root.modelData
		color: "transparent"
		exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation
		visible: root.switchedOn && root.apps.length > 0
			&& (root.overview || slideAnim.running)
		WlrLayershell.namespace: "wayland-canvas-app-bar"
		WlrLayershell.layer: WlrLayer.Top // above the canvas, below the panels
		WlrLayershell.keyboardFocus: WlrKeyboardFocus.None // never the keyboard

		// Anchored to its edge and to the leading one along it, so the
		// margin along the edge places the strip where StripLayout says;
		// the margin on the edge itself overhangs by an entry square, the
		// pocket the strip slides into (as the toolbar's).
		// Its edge, and the leading edge along it: the margin along the edge
		// is where StripLayout puts the strip, so a shared edge lines the two
		// strips up; the margin on the edge itself overhangs by an entry
		// square, the pocket the strip slides into.
		anchors.left: root.edge === "left" || !root.vertical
		anchors.right: root.edge === "right"
		anchors.top: root.edge === "top" || root.vertical
		anchors.bottom: root.edge === "bottom"
		WlrLayershell.margins.left: root.edge === "left" ? -root.entrySize
			: root.vertical ? 0 : root.along
		WlrLayershell.margins.right: root.edge === "right" ? -root.entrySize : 0
		WlrLayershell.margins.top: root.edge === "top"
			? root.nearEdge - root.pocket
			: root.vertical ? root.along : 0
		WlrLayershell.margins.bottom: root.edge === "bottom" ? -root.entrySize : 0

		implicitWidth: root.vertical
			? root.entrySize + root.pocket : root.stripLength
		implicitHeight: root.vertical
			? root.stripLength : root.entrySize + root.pocket

		// Input is the strip's alone, wherever the slide has it.
		mask: Region {
			x: content.x
			y: content.y
			width: root.vertical ? root.entrySize : root.stripLength
			height: root.vertical ? root.stripLength : root.entrySize
		}

		Item {
			id: content
			x: root.edge === "right" ? -root.slide
				: root.edge === "left" ? root.pocket + root.slide : 0
			y: root.edge === "bottom" ? -root.slide
				: root.edge === "top" ? root.pocket + root.slide : 0
			width: root.vertical ? root.entrySize : root.stripLength
			height: root.vertical ? root.stripLength : root.entrySize

			Rectangle {
				anchors.fill: parent
				color: UiTheme.panel
				border.width: 1
				border.color: UiTheme.border
			}

			// The separator between the toolbar's part and this one: a
			// hairline across the strip in the gap the two leave, drawn on
			// this side of it.
			Rectangle {
				visible: root.shared && root.toolbarLength > 0
				color: UiTheme.border
				width: root.vertical ? parent.width - 16 : 1
				height: root.vertical ? 1 : parent.height - 16
				x: root.vertical ? 8 : -Math.round(StripLayout.separator / 2)
				y: root.vertical ? -Math.round(StripLayout.separator / 2) : 8
			}

			Repeater {
				model: root.apps

				AppEntry {
					required property var modelData
					required property int index

					app: modelData
					surface: root
					x: root.vertical ? 0 : index * root.entrySize
					y: root.vertical ? index * root.entrySize : 0
					width: root.entrySize
					height: root.entrySize
				}
			}
		}
	}

	// ---- the hovered app's name ----

	PanelWindow {
		screen: root.modelData
		color: "transparent"
		exclusionMode: ExclusionMode.Ignore
		visible: root.switchedOn && root.hoveredApp != null && root.overview
		WlrLayershell.namespace: "wayland-canvas-app-bar-chrome"
		WlrLayershell.layer: WlrLayer.Overlay
		WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
		anchors {
			top: true
			bottom: true
			left: true
			right: true
		}
		mask: Region {} // nothing: the label never takes a click

		Rectangle {
			readonly property int gap: 8
			readonly property int pad: 6
			readonly property int index: root.hoveredApp != null
				? root.apps.indexOf(root.hoveredApp) : -1
			// The hovered entry's square in output coordinates, from the
			// same anchors and margins the strip commits.
			readonly property int entryX: root.edge === "right"
				? root.modelData.width - root.edgeMargin - root.entrySize
				: root.edge === "left" ? root.edgeMargin
				: root.along + Math.max(0, index) * root.entrySize
			readonly property int entryY: root.edge === "bottom"
				? root.modelData.height - root.edgeMargin - root.entrySize
				: root.edge === "top" ? root.nearEdge
				: root.along + Math.max(0, index) * root.entrySize

			visible: index >= 0
			x: root.edge === "right" ? entryX - gap - width
				: root.edge === "left" ? entryX + root.entrySize + gap
				: entryX + root.entrySize / 2 - width / 2
			y: root.edge === "top" ? entryY + root.entrySize + gap
				: root.edge === "bottom" ? entryY - gap - height
				: entryY + root.entrySize / 2 - height / 2
			width: labelText.implicitWidth + 2 * pad
			height: labelText.implicitHeight + 2 * pad
			radius: 4
			color: UiTheme.background

			PanelText {
				id: labelText
				anchors.centerIn: parent
				text: root.hoveredApp != null ? root.hoveredApp.name : ""
			}
		}
	}

	// ---- what a click does ----

	// The app whose name is shown on hover.
	property var hoveredApp: null
	// The window each app was last sent to, so a second click on an app
	// with several windows walks to the next one.
	property var walked: ({})

	// A click goes to the app's window: the focused one if it holds the
	// focus, else the one after the last this entry went to.
	function appActivated(app) {
		if (app.windows.length === 0) {
			return
		}
		var key = app.appId !== "" ? app.appId : app.name
		var next = 0
		if (app.windows.length > 1) {
			var last = walked[key]
			next = last === undefined ? 0 : (last + 1) % app.windows.length
		}
		walked[key] = next
		var id = app.windows[next].id
		api.compositor.call("window.focus", {window: id}, function(result, error) {
			// A window closed between the draw and the click: the list
			// catches up with the next event, and the view stays put.
			if (error !== undefined && error !== null && error !== "") {
				console.warn("[canvas.apps] window.focus: " + error)
			}
		})
	}

	onOverviewChanged: if (!overview) {
		hoveredApp = null
	}
}
