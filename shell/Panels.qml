pragma Singleton

import QtQuick

// The one panel that may be open at a time: opening one closes the last, so
// the bar never stacks panels. Panels register here through Panel.
QtObject {
	// The open Panel, or null.
	property var open: null

	// What an indicator click does: flip this panel, closing whichever was
	// open before.
	function toggle(panel) {
		if (open === panel) {
			panel.close()
		} else {
			if (open !== null) {
				open.close()
			}
			panel.showPanel()
			open = panel
		}
	}

	// A panel went away by any path: forget it if it was the one.
	function released(panel) {
		if (open === panel) {
			open = null
		}
	}
}
