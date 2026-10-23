import QtQuick
import WaylandCanvas.Ui

// api.theme: the desktop's one light/dark mode. The mode is read from the
// config at start and applied to the UI kit's palette (which every plugin
// colours itself from); setMode flips it, persists it as theme.mode in
// shell.json and tells the compositor at start and on every change, so the
// shell and the compositor never disagree for longer than a message
// (docs/intents/shell/theme.md). A mode the compositor reports in its
// theme event — an outside client's theme.set — is adopted the same way,
// kept in the config but never told back; at start the config's choice
// still wins, so it is pushed over whatever the compositor had.
Item {
	id: root

	required property var _config // ShellConfig
	required property var _compositor

	// "light" or "dark".
	readonly property string mode: UiTheme.mode
	// The palette for that mode: background, text, attention, muted,
	// accent, hover, fill, track, and the font and panel metrics.
	readonly property var colors: UiTheme

	// Flips the palette and keeps the mode in shell.json; true when the
	// mode actually changed.
	function apply(mode) {
		if (mode !== "light" && mode !== "dark") {
			return false
		}
		if (UiTheme.mode === mode) {
			return false
		}
		UiTheme.mode = mode
		_config.set(["theme", "mode"], mode)
		return true
	}

	function setMode(mode) {
		if (apply(mode)) {
			_compositor.call("theme.set", {mode: mode})
		}
	}

	// A mode the compositor reported: adopted as if the switch had been
	// clicked — palette flipped, kept in the config — but never told back:
	// the event came from the compositor, and an event matching the
	// shell's own mode changes nothing.
	function adopt(mode) {
		apply(mode)
	}

	// Applied when the shell starts: the config's choice (dark on anything
	// unknown, logged), and the compositor told even when it is dark, so a
	// compositor that did not start dark is corrected at once.
	function start() {
		var mode = _config.data.theme ? _config.data.theme.mode : undefined
		if (mode !== undefined && mode !== "light" && mode !== "dark") {
			console.warn("[plugins] unknown theme.mode '" + mode + "' in the config: dark")
			mode = "dark"
		}
		UiTheme.mode = mode === "light" ? "light" : "dark"
		_compositor.call("theme.set", {mode: UiTheme.mode})
	}

	Connections {
		target: root._compositor
		function onEvent(name, params) {
			if (name === "theme") {
				root.adopt(params.mode)
			}
		}
	}
}
