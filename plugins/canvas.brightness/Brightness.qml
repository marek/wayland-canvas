import Quickshell
import QtQuick
import WaylandCanvas.Ui
import "."

// Brightness: the display glyph; the wheel steps the backlight by 5 %, a left
// click opens the slider panel (the manifest names it). Hidden without a
// controllable backlight; BrightnessState holds the one value all bars share.
BarButton {
	id: root

	icon: true
	visible: BrightnessState.hasBacklight
	// The glyph shows whether several monitors are connected; the backlight
	// it steers is the one value either way.
	text: Quickshell.screens.length > 1 ? "󰍺" : "󰍹"

	onWheeled: function(notches) {
		BrightnessState.apply(BrightnessState.percent + notches * 5)
	}
}
