import Quickshell
import Quickshell.Io
import QtQuick

// One shared client of the compositor's control socket
// ($WAYLAND_CANVAS_SOCKET): it subscribes as soon as it connects, asks for
// the whole state once and keeps it current through the events, reconnects
// when the socket goes, and routes each call's response back to its caller
// (docs/intents/platform/ipc.md).
Item {
	id: root

	readonly property string path: Quickshell.env("WAYLAND_CANVAS_SOCKET") || ""
	readonly property bool available: path !== "" && socket.connected

	// The compositor's state as `state` reported it and the events have
	// updated it: outputs (with the overview flag), desktops, canvas items,
	// and the theme mode.
	property var state: ({outputs: [], desktops: [], items: [], theme: "dark"})

	// Every event, with its name and params, after the state was updated.
	signal event(string name, var params)

	property int nextId: 1
	property var callbacks: ({}) // id -> callback(result, error)
	property var pending: [] // requests queued while disconnected

	// call("method", params, function(result, error) {}): exactly one
	// response, in request order. While disconnected the request is queued
	// and goes out with the subscription once the socket is back.
	function call(method, params, callback) {
		var request = {
			id: nextId,
			method: method
		}
		if (params !== undefined) {
			request.params = params
		}
		if (callback !== undefined) {
			callbacks[String(nextId)] = callback
		}
		nextId++
		if (socket.connected) {
			send(request)
		} else {
			pending.push(request)
			if (path !== "") {
				socket.connected = true
			}
		}
	}

	function send(request) {
		socket.write(JSON.stringify(request) + "\n")
		socket.flush()
	}

	Socket {
		id: socket
		path: root.path
		connected: root.path !== ""
		parser: SplitParser {
			splitMarker: "\n"
			onRead: function(data) { root.received(data) }
		}
		onConnectionStateChanged: if (connected) {
			// Subscribe first, then take the whole state: the events that
			// land in between keep it current.
			root.send({id: root.nextId++, method: "subscribe"})
			root.call("state", undefined, applyState)
			for (var i = 0; i < root.pending.length; i++) {
				root.send(root.pending[i])
			}
			root.pending = []
		} else {
			reconnect.restart()
		}
	}

	function applyState(result) {
		if (result != null && typeof result === "object") {
			state = result
		}
	}

	function received(line) {
		var message = null
		try {
			message = JSON.parse(line)
		} catch (e) {
			return
		}
		if (message == null || typeof message !== "object") {
			return
		}
		if (message.event !== undefined) {
			handleEvent(message.event, message.params || {})
			return
		}
		var callback = callbacks[String(message.id)]
		if (callback !== undefined) {
			delete callbacks[String(message.id)]
			callback(message.result, message.error)
		}
	}

	// Kept state: each event rebuilds what it names, and the whole state
	// object is reassigned — a var property mutated in place fires no
	// change signal, so bindings on the state would never re-run.
	function handleEvent(name, params) {
		if (name === "overview") {
			state = Object.assign({}, state, {
				outputs: (state.outputs || []).map(function(output) {
					return output.name === params.output
						? Object.assign({}, output, {overview: params.active})
						: output
				})
			})
		} else if (name === "outputs") {
			state = Object.assign({}, state, {outputs: params.outputs || []})
		} else if (name === "desktops") {
			state = Object.assign({}, state, {desktops: params.desktops || []})
		} else if (name === "theme") {
			state = Object.assign({}, state, {theme: params.mode})
		} else if (name === "item.moved") {
			state = Object.assign({}, state, {
				items: (state.items || []).map(function(item) {
					return item.id === params.id
						? Object.assign({}, item, {x: params.x, y: params.y})
						: item
				})
			})
		}
		event(name, params)
	}

	// The compositor went away; come back so the next call lands.
	Timer {
		id: reconnect
		interval: 2000
		onTriggered: if (root.path !== "" && !socket.connected) socket.connected = true
	}
}
