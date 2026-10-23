import QtQuick
import WaylandCanvas.Ui

// The light/dark switch, the right-most widget of the bar: a click takes the
// other mode (docs/intents/shell/theme.md). api.theme flips the palette,
// persists the mode and tells the compositor.
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
