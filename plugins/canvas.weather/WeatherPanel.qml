import QtQuick
import WaylandCanvas.Ui

// The weather panel: the forecast for the coming days, from the same wttr.in
// reading the widget shows. Rows are a read-out — nothing here acts.
PanelCard {
	id: root

	panelWidth: 320

	readonly property var forecast: model ? model.forecast : []

	Repeater {
		model: root.forecast

		PanelRow {
			id: row
			required property var modelData
			readonly property var day: modelData
			clickable: false
			glyph: day.glyph
			label: day.name

			Text {
				anchors.right: parent.right
				anchors.rightMargin: 8
				anchors.verticalCenter: parent.verticalCenter
				text: day.min + "° / " + day.max + "°"
				color: UiTheme.muted
				font {
					family: UiTheme.fontFamily
					pixelSize: UiTheme.fontBody
				}
				renderType: Text.NativeRendering
			}
		}
	}

	PanelText {
		visible: root.forecast.length === 0
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "No forecast yet"
	}
}
