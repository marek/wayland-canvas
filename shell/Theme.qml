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

	// Panels: fixed row and section metrics, so a panel's layout (and the
	// geometry its scenario tests rely on) does not drift with the font.
	readonly property int panelPadding: 10
	readonly property int panelSpacing: 6
	readonly property int rowHeight: 32
	readonly property int sectionHeight: 18
	readonly property int detailHeight: 14
	readonly property color hover: Qt.rgba(1, 1, 1, 0.08)
}
