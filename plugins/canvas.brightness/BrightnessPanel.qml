import QtQuick
import WaylandCanvas.Ui

// The brightness panel: one slider for the backlight. The widget owns the
// brightnessctl plumbing; the panel only drives it through `apply`.
PanelCard {
	id: root

	panelWidth: 240
	panelName: "brightness"

	readonly property int percent: model ? model.percent : 0

	PanelText {
		width: parent.width
		height: UiTheme.sectionHeight
		text: "Brightness — " + root.percent + "%"
	}

	PanelSlider {
		value: root.percent / 100
		onMoved: function(v) {
			if (root.model != null) {
				root.model.apply(Math.round(v * 100))
			}
		}
	}
}
