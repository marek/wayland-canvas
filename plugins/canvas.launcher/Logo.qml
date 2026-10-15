import Quickshell.Io
import QtQuick
import WaylandCanvas.Ui

// The logo button: opens the compositor's own launcher through the control
// socket; right-click opens a terminal.
BarButton {
	id: root

	icon: true
	text: "\uf17a"

	Process {
		id: terminal
		command: ["/bin/sh", "-c", "exec ${TERMINAL:-foot}"]
	}

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			terminal.running = true
		} else if (api != null) {
			api.compositor.call("launcher.toggle")
		}
	}
}
