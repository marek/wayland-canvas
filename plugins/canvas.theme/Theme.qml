import QtQuick
import WaylandCanvas.Ui

// The light/dark switch, the right-most widget of the bar: a sun in dark
// mode, a moon in light mode, a click takes the other mode
// (docs/intents/shell/theme.md). api.theme does the rest — flips the UI
// kit's palette the whole shell colours itself from, persists the mode as
// theme.mode in shell.json and tells the compositor — so this plugin is
// only the glyph and the call; another plugin wanting its own switch reads
// the same api.
BarButton {
	id: root

	icon: true
	// The sun offers the light mode; the moon offers the dark one — the
	// glyph names what a click brings, not what is on screen.
	text: api != null && api.theme.mode === "dark" ? "\ue30d" : "\ue32b"

	onPressed: function(button) {
		if (button === Qt.LeftButton && api != null) {
			api.theme.setMode(api.theme.mode === "dark" ? "light" : "dark")
		}
	}
}
