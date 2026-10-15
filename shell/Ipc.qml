import Quickshell
import Quickshell.Io
import QtQuick

// Client for the compositor's control socket: one-line commands in, one-line
// responses out (ignored — the compositor's state is the answer).
Item {
	id: root

	property string path: Quickshell.env("WAYLAND_CANVAS_SOCKET") || ""
	readonly property bool available: path !== "" && socket.connected

	function send(command) {
		if (socket.connected) {
			socket.write(command + "\n")
			socket.flush()
		} else {
			// The compositor went away; reconnect so the next click lands.
			socket.connected = true
		}
	}

	Socket {
		id: socket
		path: root.path
		connected: root.path !== ""
		parser: SplitParser {
			splitMarker: "\n"
		}
		onConnectionStateChanged: if (!connected) reconnect.restart()
	}

	Timer {
		id: reconnect
		interval: 2000
		onTriggered: if (root.path !== "" && !socket.connected) socket.connected = true
	}
}
