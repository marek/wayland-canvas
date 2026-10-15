import QtQuick

// The bar's own palette and metrics: a dark theme matched to the reference
// look. Icons are glyphs of the monospace font, not images.
QtObject {
	readonly property color background: "#1a1b26"
	readonly property color text: "#a9b1d6"
	readonly property color attention: "#f7768e"
	readonly property color muted: "#565f89"
	readonly property color accent: "#7aa2f7"

	readonly property string fontFamily: "monospace"
	readonly property int fontBody: 12
	readonly property int fontIcon: 13
	readonly property int barHeight: 26
	readonly property int iconSlot: 27
	readonly property int iconSize: 16
	readonly property int edgeMargin: 8
}
