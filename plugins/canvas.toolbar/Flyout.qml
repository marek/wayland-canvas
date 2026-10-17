import QtQuick
import WaylandCanvas.Ui
import "."

// A menu button's flyout: rows of a glyph or a colour swatch and a
// label, on the toolbar's inner side toward the screen's centre, level
// with its button along the strip, kept inside the output. It draws on
// the chrome surface, whose window spans the output, so its coordinates
// are the output's. Choosing a row runs that entry's action and folds
// the flyout; a second click of the button or a click outside (the
// surface catches those) folds it too.
Rectangle {
	id: root

	property var surface: null // the toolbar surface
	property var api: null // its api, for running the entries' actions
	property var button: null // the menu button this flyout hangs from

	color: UiTheme.panel // a card, like the strip it hangs from
	border.width: 1
	border.color: UiTheme.border
	width: surface != null ? surface.flyoutWidth : 0
	height: column.implicitHeight + 2 * UiTheme.panelPadding

	// The button's square, from the toolbar's layout of the strip.
	readonly property var box: button != null && button.index >= 0
		&& surface != null ? surface.buttonBox(button.index) : null

	// Beside the strip on its inner side, one margin away from it; level
	// with the button along the strip — the card's near edge sits at the
	// button's near one, hanging along the edge — kept inside the output.
	x: {
		if (box == null || surface == null) {
			return 0
		}
		var want = 0
		if (surface.edge === "right") {
			want = box.x - UiTheme.edgeMargin - width
		} else if (surface.edge === "left") {
			want = box.x + box.width + UiTheme.edgeMargin
		} else {
			want = box.x
		}
		return Math.round(Math.max(UiTheme.edgeMargin, Math.min(want,
			surface.modelData.width - width - UiTheme.edgeMargin)))
	}
	y: {
		if (box == null || surface == null) {
			return 0
		}
		var want = 0
		if (surface.edge === "top") {
			want = box.y + box.height + UiTheme.edgeMargin
		} else if (surface.edge === "bottom") {
			want = box.y - UiTheme.edgeMargin - height
		} else {
			want = box.y
		}
		return Math.round(Math.max(UiTheme.edgeMargin, Math.min(want,
			surface.modelData.height - height - UiTheme.edgeMargin)))
	}

	// Swallow the card's own padding: only a click truly outside folds.
	MouseArea {
		anchors.fill: parent
	}

	Column {
		id: column
		x: UiTheme.panelPadding
		y: UiTheme.panelPadding
		width: parent.width - 2 * UiTheme.panelPadding
		spacing: UiTheme.panelSpacing

		Repeater {
			model: root.button != null ? root.button.spec.menu : []

			FlyoutRow {
				required property var modelData
				entry: modelData
				onChosen: {
					if (root.api != null && entry.action !== "") {
						root.api.actions.run(entry.action, entry.args)
					}
					root.surface.foldFlyout()
				}
			}
		}
	}
}
