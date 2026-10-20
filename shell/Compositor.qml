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
	// updated it: outputs (with the overview flag), desktops, windows, the
	// theme mode, and the recording (null, or an object with the file's
	// path).
	property var state: ({outputs: [], desktops: [], windows: [], theme: "dark", recording: null})

	// Every event, with its name and params, after the state was updated.
	signal event(string name, var params)

	property int nextId: 1
	property var callbacks: ({}) // id -> callback(result, error)
	property var pending: [] // requests queued while disconnected

	// call("method", params, function(result, error) {}): exactly one
	// response, in request order, with `error` null or text. While
	// disconnected the request is queued and goes out with the subscription
	// once the socket is back.
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
			root.failInFlight()
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
		answer(String(message.id), message.result,
			message.error === undefined ? null : String(message.error))
	}

	// Hands one call its answer, once. `error` is null, or the text of what
	// went wrong (the compositor's own, or "disconnected"), and `result` is
	// null then.
	function answer(id, result, error) {
		var callback = callbacks[id]
		if (callback === undefined) {
			return
		}
		delete callbacks[id]
		callback(error === null ? result : null, error)
	}

	// Every call already sent and not yet answered fails: its answer went
	// down with the socket. A call queued while disconnected goes out on the
	// next connection and keeps waiting.
	function failInFlight() {
		var queued = {}
		for (var i = 0; i < pending.length; i++) {
			queued[String(pending[i].id)] = true
		}
		var ids = Object.keys(callbacks).filter(function(id) { return queued[id] !== true })
		for (var j = 0; j < ids.length; j++) {
			answer(ids[j], null, "disconnected")
		}
	}

	// How each event updates the state, by event name: a function of the
	// state and the event's params returning the fields it replaces. An
	// event named nowhere here (action, settings) changes no state.
	readonly property var stateUpdates: ({
		"overview": function(current, params) {
			return {outputs: (current.outputs || []).map(function(output) {
				return output.name === params.output
					? Object.assign({}, output, {overview: params.active})
					: output
			})}
		},
		"outputs": function(current, params) { return {outputs: params.outputs || []} },
		"desktops": function(current, params) { return {desktops: params.desktops || []} },
		"windows": function(current, params) { return {windows: params.windows || []} },
		"theme": function(current, params) { return {theme: params.mode} },
		"capture.recording": function(current, params) {
			return {recording: params.active === true ? {path: params.path || ""} : null}
		}
	})

	// The whole state object is reassigned: a var property mutated in
	// place fires no change signal, so bindings on the state would never
	// re-run.
	function handleEvent(name, params) {
		var update = stateUpdates[name]
		if (update !== undefined) {
			state = Object.assign({}, state, update(state, params))
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
