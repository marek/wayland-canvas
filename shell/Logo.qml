import Quickshell.Io
import QtQuick

// The logo button: opens the compositor's own launcher through the control
// socket; right-click opens a terminal.
BarButton {
	id: root

	icon: true
	text: "\uf17a"

	Ipc {
		id: ipc
	}

	Process {
		id: terminal
		command: ["/bin/sh", "-c", "exec ${TERMINAL:-foot}"]
	}

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			terminal.running = true
		} else {
			ipc.send("launcher")
		}
	}
}
