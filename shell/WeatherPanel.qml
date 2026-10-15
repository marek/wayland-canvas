import QtQuick

// The weather panel: the forecast for the coming days, from the same wttr.in
// reading the widget shows. Rows are a read-out — nothing here acts.
Panel {
	id: root

	panelWidth: 320
	panelName: "weather"
	property var model: null // the weather widget: forecast

	readonly property var forecast: model ? model.forecast : []

	Repeater {
		model: root.forecast

		PanelRow {
			id: row
			required property var modelData
			readonly property var day: modelData
			theme: root.theme
			clickable: false
			glyph: day.glyph
			label: day.name

			Text {
				anchors.right: parent.right
				anchors.rightMargin: 8
				anchors.verticalCenter: parent.verticalCenter
				text: day.min + "° / " + day.max + "°"
				color: root.theme ? root.theme.muted : "#565f89"
				font {
					family: root.theme ? root.theme.fontFamily : "monospace"
					pixelSize: root.theme ? root.theme.fontBody : 12
				}
				renderType: Text.NativeRendering
			}
		}
	}

	PanelText {
		visible: root.forecast.length === 0
		theme: root.theme
		dim: true
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "No forecast yet"
	}
}
