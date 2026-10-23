import QtQuick
import WaylandCanvas.Ui
import "."

// The weather panel: the forecast for the coming days, from the same wttr.in
// reading the widget shows. Rows are a read-out — nothing here acts. While
// it is open the weather is watched, so a panel opened with no widget up
// still has a forecast to show.
PanelCard {
	id: root

	panelWidth: 320

	readonly property var forecast: WeatherState.forecast

	onOpenChanged: {
		if (open) {
			WeatherState.watch()
		} else {
			WeatherState.unwatch()
		}
	}

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
