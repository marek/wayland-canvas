pragma Singleton
import QtQuick

// Where the overview's two strips sit on an edge they share: the toolbar's
// buttons first, a separator, then the app bar's apps
// (docs/intents/shell/app-bar.md). Both surfaces place themselves from these
// functions, so the pair reads as one strip without either plugin knowing
// the other: each is given the lengths, which both read off the shell config,
// the toolbar registry and the compositor's window list.
QtObject {
	// The gap between the two strips, with the separator line drawn in it.
	readonly property int separator: 9

	// The whole strip's length on its edge: both parts and the gap, or just
	// the one part that is there.
	function totalLength(toolbarLength, appsLength) {
		if (toolbarLength <= 0 || appsLength <= 0) {
			return toolbarLength + appsLength
		}
		return toolbarLength + separator + appsLength
	}

	// Where the toolbar's part starts, measured along the edge from the
	// screen's leading corner, with the pair centred as one.
	function toolbarStart(screenLength, toolbarLength, appsLength) {
		return Math.round((screenLength - totalLength(toolbarLength, appsLength)) / 2)
	}

	// Where the app bar's part starts: after the toolbar's and the gap, else
	// centred on its own.
	function appsStart(screenLength, toolbarLength, appsLength) {
		var start = toolbarStart(screenLength, toolbarLength, appsLength)
		return toolbarLength > 0 ? start + toolbarLength + separator : start
	}
}
