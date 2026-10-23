import QtQuick

// The settings window's model: canvas.conf as the compositor reports it
// (`settings.get`, kept current by the `settings` event), the chord
// recording that rides the same socket, the commands list's draft, and the
// one write that changes the file, `set`. The tabs read this and write
// through it; none of them speaks to the socket itself.
Item {
	id: root

	property var api: null
	property bool open: false // the window is up; closing cancels a recording

	visible: false

	// ---- the settings object: asked once, kept current by the event ----

	property var settings: null // the settings object (docs/intents/platform/ipc.md)
	property string settingsError: "" // settings.get itself failed

	function assignSettings(result) {
		var next = {}
		for (var key in result) {
			if (key !== "moved") {
				next[key] = result[key]
			}
		}
		settings = next
	}

	// What a reply's error says, "" when the call went through.
	function errorText(error) {
		return error === undefined || error === null ? "" : String(error)
	}

	// The panel's api arrives after the instance is created (the host
	// hands it over once its own wiring is done), so the first read rides
	// the opening, not the completion. Closing cancels a recording under
	// way: the compositor never holds the keyboard for a closed window.
	onOpenChanged: {
		if (open) {
			refresh()
		} else if (recording != null) {
			cancelRecording()
		}
	}

	function refresh() {
		api.compositor.call("settings.get", undefined, function(result, error) {
			settingsError = errorText(error)
			if (settingsError === "") {
				assignSettings(result)
			}
		})
	}

	Connections {
		target: root.api.compositor
		function onEvent(name, params) {
			if (name === "settings") {
				root.settingsError = ""
				root.settings = params
			}
		}
	}

	// ---- recording a chord ----

	property var recording: null // {scope, action} while a capture runs

	function cancelRecording() {
		if (recording == null) {
			return
		}
		recording = null
		api.compositor.call("shortcut.cancel", undefined, function() {})
	}

	// Arms the capture for one row; a new call supersedes the one under
	// way (the compositor cancels it), and its late answer is dropped.
	function capture(scope, action, onKeys) {
		cancelRecording()
		recording = {scope: scope, action: action}
		api.compositor.call("shortcut.capture", {scope: scope}, function(result, error) {
			if (recording == null || recording.scope !== scope
					|| recording.action !== action) {
				return
			}
			recording = null
			var fault = errorText(error)
			if (fault !== "") {
				onKeys(null, fault)
				return
			}
			onKeys(result != null ? result.keys : null, "")
		})
	}

	// ---- writing canvas.conf ----

	// The fault line and the moved-chord line: one row each, the latest.
	property var faultRow: null // {scope, action} or {what} or {scope: "commands"}
	property string faultText: ""
	property var movedRow: null // {scope, action} of the last successful set
	property string movedText: ""

	function rowFault(entry) {
		return faultRow != null && faultRow.scope === entry.scope
			&& faultRow.action === entry.action ? faultText : ""
	}

	function rowNote(entry) {
		return movedRow != null && movedRow.scope === entry.scope
			&& movedRow.action === entry.action ? movedText : ""
	}

	// The fault line of a [gestures] or [session] setting, by its param.
	function fieldFault(what) {
		return faultRow != null && faultRow.what === what ? faultText : ""
	}

	// Where a moved chord came from: the loser action's label, or the
	// command text when the loser was a [commands] entry.
	function movedFrom(moved) {
		var shortcuts = settings && settings.shortcuts ? settings.shortcuts : []
		for (var i = 0; i < shortcuts.length; i++) {
			if (shortcuts[i].scope === moved.scope
					&& shortcuts[i].action === moved.action) {
				return shortcuts[i].label
			}
		}
		return "the command " + (moved.command || "")
	}

	function fail(faultKey, text) {
		faultRow = faultKey
		faultText = text
	}

	// One write to canvas.conf. A failure shows on the row `faultKey` names
	// ({scope, action}, {scope: "commands"} or {what}); an answer replaces
	// the settings and clears the fault line, and `then` sees it.
	function set(params, faultKey, then) {
		api.compositor.call("settings.set", params, function(result, error) {
			var fault = errorText(error)
			if (fault !== "") {
				fail(faultKey, fault)
				return
			}
			faultRow = null
			assignSettings(result)
			if (then !== undefined) {
				then(result)
			}
		})
	}

	// One setting of [gestures] or [session], by its param in settings.get.
	function setField(what, value) {
		var params = {}
		params[what] = value
		set(params, {what: what})
	}

	function setKeys(scope, action, keys) {
		set({scope: scope, action: action, keys: keys}, {scope: scope, action: action},
			function(result) {
				movedRow = {scope: scope, action: action}
				movedText = ""
				var moved = result != null ? result.moved : null
				for (var i = 0; moved != null && i < moved.length; i++) {
					movedText += (movedText === "" ? "" : "  ")
						+ (moved[i].keys || []).join(", ") + " taken from "
						+ movedFrom(moved[i])
				}
			})
	}

	// ---- the commands, one section of the Shortcuts tab ----

	// A command being added: the file never holds half of it. The chord
	// and the command line wait here until both are given; one write
	// then adds the line.
	property var draft: null // {keys, command}, either still ""

	function commands() {
		var real = settings && settings.commands ? settings.commands : []
		return draft != null ? real.concat([draft]) : real
	}

	function commandCount() {
		return settings && settings.commands ? settings.commands.length : 0
	}

	function clearCommandsFault() {
		if (faultRow != null && faultRow.scope === "commands") {
			faultRow = null
		}
	}

	function saveCommands(commands) {
		set({commands: commands}, {scope: "commands"})
	}

	// The draft's command line is what the field holds when its recording
	// starts: set in place, the row stays up with the text in it until
	// the chord arrives.
	function holdDraftText(index, text) {
		if (draft != null && index === commandCount()) {
			draft.command = text
		}
	}

	function commandKeys(index, keys) {
		var commands = settings && settings.commands ? settings.commands.slice() : []
		if (draft != null && index === commands.length) {
			clearCommandsFault()
			draft = {keys: keys, command: draft.command}
			if (draft.command.trim() !== "") {
				commands.push({keys: keys, command: draft.command})
				saveCommands(commands)
			}
			return
		}
		if (index < commands.length) {
			commands[index] = {keys: keys, command: commands[index].command}
			saveCommands(commands)
		}
	}

	function commandText(index, text) {
		if (text.trim() === "") {
			return // an emptied field is not a command; the file keeps its line
		}
		var commands = settings && settings.commands ? settings.commands.slice() : []
		if (draft != null && index === commands.length) {
			draft = {keys: draft.keys, command: text}
			if (draft.keys == null || draft.keys === "") {
				fail({scope: "commands"}, "Record a chord first")
				return
			}
			commands.push({keys: draft.keys, command: text})
			saveCommands(commands)
			return
		}
		if (index < commands.length) {
			commands[index] = {keys: commands[index].keys, command: text}
			saveCommands(commands)
		}
	}

	// A recorded command lands in the file: the draft it filled is the
	// real row now. Watch the settings, not the draft, for when that is.
	onSettingsChanged: if (draft != null && draft.keys != null && draft.keys !== "") {
		var real = settings && settings.commands ? settings.commands : []
		for (var i = 0; i < real.length; i++) {
			if (real[i].keys === draft.keys) {
				draft = null
				break
			}
		}
	}

	function removeCommand(index) {
		if (draft != null) {
			if (index === commandCount()) {
				draft = null
				return
			}
			draft = null // a remove also drops an unfilled draft
		}
		var commands = settings && settings.commands ? settings.commands.slice() : []
		if (index < commands.length) {
			commands.splice(index, 1)
			saveCommands(commands)
		}
	}
}
