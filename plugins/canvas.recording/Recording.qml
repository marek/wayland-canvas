import Quickshell
import QtQuick
import WaylandCanvas.Ui

// The recording dot: while a screen recording runs, a red dot sits in the
// bar; a click stops it. It follows the `capture.recording` events, and a
// shell that comes up mid-recording takes its word from the state instead.
// BarButton hides itself when its text is empty, so no recording means no
// dot.
BarButton {
	id: root

	property bool recording: false
	property string path: ""

	// The state as it was when this widget came up: `recording` is null
	// while nothing runs, else an object with the file's path.
	Component.onCompleted: {
		var state = api != null && api.compositor != null
			? api.compositor.state : null
		if (state != null && state.recording != null) {
			recording = true
			path = state.recording.path || ""
		}
	}

	Connections {
		target: root.api != null ? root.api.compositor : null
		function onEvent(name, params) {
			if (name === "capture.recording") {
				root.recording = params.active === true
				root.path = params.path || ""
			}
		}
	}

	text: recording ? "\uf111" : "" // the dot; empty hides the button
	icon: true
	active: recording // the attention colour paints it red

	onPressed: function(button) {
		if (button === Qt.LeftButton && recording) {
			api.compositor.call("capture.stop")
		}
	}
}
