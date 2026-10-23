import Quickshell
import QtQuick
import WaylandCanvas.Ui

// The recording dot: a red dot in the bar while a screen recording runs; a
// click stops it. It reads the compositor's `recording` state (null, or
// {path}), which a shell that comes up mid-recording already holds.
// BarButton hides itself when its text is empty.
BarButton {
	id: root

	readonly property bool recording: api != null && api.compositor != null
		&& api.compositor.state.recording != null

	text: recording ? "\uf111" : "" // the dot; empty hides the button
	icon: true
	active: recording // the attention colour paints it red

	onPressed: function(button) {
		if (button === Qt.LeftButton && recording) {
			api.compositor.call("capture.stop")
		}
	}
}
