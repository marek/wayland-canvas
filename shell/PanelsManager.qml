import QtQuick

// The common trigger behind every panel (api.panels): opening a panel is
// one call by its global name, any widget, button or action can open any
// plugin's panel, one panel is open at a time, and it closes on a click
// outside it, on Escape or on a second toggle of the same widget. A toggle
// for the same panel from a different widget (another tray icon) re-anchors
// it instead, so a family of items can share one menu.
Item {
	id: root

	// The global name ("<plugin id>/<panel id>") of the open panel, "" when
	// none is open.
	property string openName: ""
	property var openAnchor: null

	property var panels: ({}) // global name -> the hidden PanelCard instance
	property var warned: ({}) // names already logged as missing

	// The host registers each manifest panel entry's instance here.
	function register(name, instance) {
		panels[name] = instance
		instance.dismissed.connect(function() { forget(name) })
	}

	// What a click on a widget or button does: flip this panel, closing
	// whichever was open before.
	function toggle(name, anchor) {
		if (name === openName && anchor === openAnchor) {
			close()
			return
		}
		open(name, anchor)
	}

	function open(name, anchor) {
		var instance = panels[name]
		if (instance == null) {
			if (!warned[name]) {
				warned[name] = true
				console.warn("[plugins] no panel named " + name)
			}
			return
		}
		if (openName !== "" && openName !== name) {
			close()
		}
		instance.anchorItem = anchor
		instance.showPanel()
		openName = name
		openAnchor = anchor
	}

	function close() {
		if (openName === "") {
			return
		}
		var instance = panels[openName]
		openName = ""
		openAnchor = null
		if (instance != null) {
			instance.close()
		}
	}

	// A panel went away by any path (its own dismissal, a reload that
	// dropped it): forget it if it was the one.
	function forget(name) {
		if (name === openName) {
			openName = ""
			openAnchor = null
		}
	}
}
