import Quickshell
import QtQuick
import WaylandCanvas.Ui
import "."

// Brightness: the display glyph; the wheel steps the backlight by 5 %, a
// left click opens the slider panel (the manifest names it). Hidden without
// a controllable backlight. The backlight is BrightnessState's: every bar's
// widget and the panel show and set the one value.
BarButton {
	id: root

	icon: true
	visible: BrightnessState.hasBacklight
	text: Quickshell.screens.length > 1 ? "󰍺" : "󰍹"

	onWheeled: function(notches) {
		BrightnessState.apply(BrightnessState.percent + notches * 5)
	}
}
