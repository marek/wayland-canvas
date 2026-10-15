import QtQuick
import WaylandCanvas.Ui

// api.theme: the desktop's one light/dark mode. The mode is read from the
// config at start and applied to the UI kit's palette (which every plugin
// colours itself from); setMode flips it, persists it as theme.mode in
// shell.json and tells the compositor at start and on every change, so the
// shell and the compositor never disagree for longer than a message
// (docs/intents/shell/theme.md).
Item {
	id: root

	property var host: null

	// "light" or "dark".
	readonly property string mode: UiTheme.mode
	// The palette for that mode: background, text, attention, muted,
	// accent, hover, fill, track, and the font and panel metrics.
	readonly property var colors: UiTheme

	function setMode(mode) {
		if (mode !== "light" && mode !== "dark") {
			return
		}
		if (UiTheme.mode === mode) {
			return
		}
		UiTheme.mode = mode
		host.config.theme = host.config.theme || {}
		host.config.theme.mode = mode
		host.writeConfig()
		host.compositorApi.call("theme.set", {mode: mode})
	}

	// Applied when the shell starts: the config's choice (dark on anything
	// unknown, logged), and the compositor told even when it is dark, so a
	// compositor that did not start dark is corrected at once.
	function start() {
		var mode = host.config.theme ? host.config.theme.mode : undefined
		if (mode !== undefined && mode !== "light" && mode !== "dark") {
			console.warn("[plugins] unknown theme.mode '" + mode + "' in the config: dark")
			mode = "dark"
		}
		UiTheme.mode = mode === "light" ? "light" : "dark"
		host.compositorApi.call("theme.set", {mode: UiTheme.mode})
	}
}
