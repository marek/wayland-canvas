import Quickshell
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
// The strip, its slide, its edge and its chrome are the UI kit's EdgeStrip.
// On the toolbar's own edge the two read as one strip, the toolbar's buttons
// first, the separator in the gap, the apps last: each publishes its length
// and StripLayout places them, so neither surface knows the other.
EdgeStrip {
	id: root

	edge: api.shell.appBarPosition
	namespace: "wayland-canvas-app-bar"
	switchedOn: api.shell.appBarEnabled
	count: apps.length
	order: 1
	hoverIndex: indexOfKey(hoveredKey)
	label: {
		var app = appFor(hoveredKey)
		return app != null ? app.name : ""
	}
	catching: flyoutKey !== ""
	onDismissed: foldFlyout()

	// ---- the apps ----

	// One entry per app, in the order the apps' first windows opened: the
	// window list is in that order already. Each entry keeps a stable key
	// (the app id), its windows newest-first, and what its desktop entry
	// says — the icon and the name a user knows the app by.
	readonly property var apps: groupWindows()

	function groupWindows() {
		var windows = api.compositor != null
			? (api.compositor.state.windows || []) : []
		var byApp = ({})
		var out = []
		for (var i = 0; i < windows.length; i++) {
			var w = windows[i]
			var key = w.appId !== "" ? w.appId : ("window:" + w.id)
			if (byApp[key] === undefined) {
				byApp[key] = {
					key: key,
					appId: w.appId,
					name: entryName(w),
					icon: entryIcon(w.appId),
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

	// The app's desktop entry, by its app id: the name and icon come from
	// it, as the launcher's do (docs/intents/apps/desktop-entries.md).
	function desktopEntry(appId) {
		if (appId === "") {
			return null
		}
		var entry = DesktopEntries.byId(appId)
		return entry != null ? entry : DesktopEntries.heuristicLookup(appId)
	}

	function entryName(w) {
		var entry = desktopEntry(w.appId)
		if (entry != null && entry.name !== "") {
			return entry.name
		}
		return w.appId !== "" ? w.appId : w.title
	}

	function entryIcon(appId) {
		var entry = desktopEntry(appId)
		if (entry == null || entry.icon === "") {
			return ""
		}
		return Quickshell.iconPath(entry.icon, true)
	}

	// The entry with `key`, as the list reads now: the list is rebuilt on
	// every state change, so nothing may hold on to an entry object.
	function appFor(key) {
		var index = indexOfKey(key)
		return index >= 0 ? apps[index] : null
	}

	function indexOfKey(key) {
		for (var i = 0; i < apps.length; i++) {
			if (apps[i].key === key) {
				return i
			}
		}
		return -1
	}

	Repeater {
		model: root.apps

		AppEntry {
			required property var modelData

			app: modelData
			surface: root
		}
	}

	// ---- the window flyout ----

	// An app's windows, listed beside its entry: one row per window, the
	// focused one marked. A click goes to that window.
	flyouts: Rectangle {
		readonly property var app: root.appFor(root.flyoutKey)
		readonly property var box: root.squareBox(root.indexOfKey(root.flyoutKey))
		readonly property var spot: box != null
			? root.placeBeside(box, width, height, false, UiTheme.edgeMargin)
			: Qt.point(0, 0)

		visible: app != null && box != null
		width: 240
		height: rows.implicitHeight + 2 * UiTheme.panelPadding
		color: UiTheme.panel
		border.width: 1
		border.color: UiTheme.border
		radius: 4
		x: spot.x
		y: spot.y

		// Swallow the card's own padding: only a click truly outside folds.
		MouseArea {
			anchors.fill: parent
		}

		Column {
			id: rows
			anchors.fill: parent
			anchors.margins: UiTheme.panelPadding
			spacing: 2

			Repeater {
				model: parent.parent.app != null ? parent.parent.app.windows : []

				PanelRow {
					required property var modelData

					// Nothing is parked on the right of these rows: the
					// title may use the card's full width, elided in it.
					reserve: 8
					label: modelData.title !== "" ? modelData.title
						: (modelData.appId !== "" ? modelData.appId : "A window")
					marked: modelData.focused === true
					detail: modelData.minimized === true ? "Minimized" : ""
					onClicked: root.goToWindow(modelData.id)
				}
			}
		}
	}

	// ---- what a click does ----

	// The entry the pointer is over, and the one whose window list is open,
	// both by key: the grouped list is rebuilt on every state change, so an
	// entry object held across one would no longer be in it.
	property string hoveredKey: ""
	property string flyoutKey: ""

	// A click on an entry: one window goes straight there, several unfold
	// the list of them, and a second click on the entry folds it again.
	function appActivated(app) {
		if (app.windows.length === 0) {
			return
		}
		if (app.windows.length === 1) {
			goToWindow(app.windows[0].id)
			return
		}
		flyoutKey = flyoutKey === app.key ? "" : app.key
	}

	function foldFlyout() {
		flyoutKey = ""
	}

	// The compositor focuses the window and moves the view to it: its
	// desktop, else a view centred on it (docs/intents/platform/ipc.md).
	function goToWindow(id) {
		foldFlyout()
		api.compositor.call("window.focus", {window: id}, function(result, error) {
			// A window closed between the draw and the click: the list
			// catches up with the next event, and the view stays put.
			if (error !== null) {
				console.warn("[canvas.apps] window.focus: " + error)
			}
		})
	}

	onOverviewChanged: if (!overview) {
		hoveredKey = ""
		flyoutKey = ""
	}
}
