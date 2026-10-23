import QtQuick
import WaylandCanvas.Ui

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

	// Beside the strip, one margin away from it, the card's near edge at
	// the button's near one and hanging along the strip.
	readonly property var spot: button != null && surface != null
		? surface.placeBeside(surface.squareBox(button.index), width, height,
			false, UiTheme.edgeMargin)
		: Qt.point(0, 0)
	x: spot.x
	y: spot.y

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

			PanelRow {
				required property var modelData
				glyph: modelData.icon !== undefined ? modelData.icon : ""
				swatch: modelData.color !== undefined ? modelData.color : ""
				label: modelData.label !== undefined ? modelData.label : ""
				onClicked: {
					if (root.api != null && modelData.action !== "") {
						root.api.actions.run(modelData.action, modelData.args)
					}
					root.surface.foldFlyout()
				}
			}
		}
	}
}
