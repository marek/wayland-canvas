pragma Singleton

import QtQuick

// The shell's palette and metrics, in both modes: the dark theme the bar
// was born with and its light match (docs/intents/shell/theme.md). The host
// owns the mode — api.theme sets it — and every UI-kit piece and plugin
// that draws reads its colours from here, so the whole shell follows one
// switch. Icons are glyphs of the monospace font, not images.
QtObject {
	// "light" or "dark"; set by the host at start and on every change.
	property string mode: "dark"

	readonly property bool light: mode === "light"

	// The five named colours of each mode. Text and detail greys stay
	// readable on both backgrounds; the accent is the one saturated fill,
	// and onAccent is the text that sits on it (light text on the light
	// mode's dark blue, near-black on the dark mode's light blue).
	readonly property color background: light ? "#e1e2e7" : "#1a1b26"
	readonly property color text: light ? "#3760bf" : "#a9b1d6"
	readonly property color attention: light ? "#f52a65" : "#f7768e"
	readonly property color muted: light ? "#8489a3" : "#565f89"
	readonly property color accent: light ? "#2e7de9" : "#7aa2f7"
	readonly property color onAccent: light ? "#ffffff" : "#1a1b26"

	// Panel surfaces: the card a panel floats above whatever is under it —
	// the background's own colour in dark, one step lighter in light so a
	// card reads as raised over the bar and the windows. The border is the
	// hairline that keeps a light card off a light window and a dark card
	// off a dark wallpaper.
	readonly property color panel: light ? "#f4f5f8" : "#1a1b26"
	readonly property color border: light ? Qt.rgba(0, 0, 0, 0.12) : Qt.rgba(1, 1, 1, 0.10)

	// A selected row or day: the accent held back to a tint, so selection
	// reads as the accent's family without shouting like a fill.
	readonly property color selected: Qt.rgba(accent.r, accent.g, accent.b, light ? 0.16 : 0.22)

	// Subtle overlays on the surface below: a hovered row, a field's fill,
	// an off switch's track.
	readonly property color hover: light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.08)
	readonly property color fill: light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(1, 1, 1, 0.06)
	readonly property color track: light ? Qt.rgba(0, 0, 0, 0.14) : Qt.rgba(1, 1, 1, 0.14)

	readonly property string fontFamily: "monospace"
	readonly property int fontBody: 12
	readonly property int fontSmall: 11 // section headers, a hero's meta line
	readonly property int fontTitle: 15 // a hero's title
	readonly property int fontIcon: 13
	readonly property int barHeight: 26
	readonly property int iconSlot: 27
	readonly property int iconSize: 16
	readonly property int edgeMargin: 8

	// Panels: fixed row and section metrics, so a panel's layout (and the
	// geometry its scenario tests rely on) does not drift with the font.
	readonly property int panelPadding: 10
	readonly property int panelSpacing: 6
	readonly property int rowHeight: 32
	readonly property int sectionHeight: 18
	readonly property int detailHeight: 14
	readonly property int heroHeight: 44
	readonly property int heroIcon: 32
}
