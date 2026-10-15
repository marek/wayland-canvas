import Quickshell
import QtQuick
import WaylandCanvas.Ui
import "."

// A menu button's flyout: rows of a glyph or a colour swatch and a
// label, expanding out of the toolbar to the left of its button.
// Choosing a row runs that entry's action and folds the flyout; a
// second click of the button or a click outside (the surface catches
// those) folds it too.
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

	TransformWatcher {
		id: watcher
		a: root.button != null && root.button.QsWindow.window != null
			? root.button.QsWindow.window.contentItem : null
		b: root.button
	}

	// To the left of the button, one margin away from the strip, centred
	// on it, kept inside the output.
	x: {
		watcher.transform
		return button != null
			? Math.round(button.mapToItem(null, 0, 0).x
				- UiTheme.edgeMargin - width) : 0
	}
	y: {
		watcher.transform
		if (button == null) {
			return 0
		}
		var top = button.mapToItem(null, 0, height / 2).y - height / 2
		return Math.round(Math.max(UiTheme.edgeMargin,
			Math.min(top, surface.height - height - UiTheme.edgeMargin)))
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
