import QtQuick

// The brightness panel: one slider for the backlight. The widget owns the
// brightnessctl plumbing; the panel only drives it through `apply`.
Panel {
	id: root

	panelWidth: 240
	panelName: "brightness"
	property var model: null // the brightness widget: percent, apply

	readonly property int percent: model ? model.percent : 0

	PanelText {
		theme: root.theme
		width: parent.width
		height: root.theme ? root.theme.sectionHeight : 18
		text: "Brightness — " + root.percent + "%"
	}

	PanelSlider {
		theme: root.theme
		value: root.percent / 100
		onMoved: function(v) {
			if (root.model != null) {
				root.model.apply(Math.round(v * 100))
			}
		}
	}
}
