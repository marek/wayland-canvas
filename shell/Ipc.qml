import Quickshell
import Quickshell.Io
import QtQuick

// Client for the compositor's control socket: JSON-line requests in,
// responses and events out. Responses are ignored — the compositor's state
// is the answer.
Item {
	id: root

	property string path: Quickshell.env("WAYLAND_CANVAS_SOCKET") || ""
	readonly property bool available: path !== "" && socket.connected

	property int nextId: 1

	function send(method, params) {
		const request = {
			id: nextId++,
			method: method
		}
		if (params !== undefined)
			request.params = params
		if (socket.connected) {
			socket.write(JSON.stringify(request) + "\n")
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
