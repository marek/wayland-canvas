import QtQuick
import WaylandCanvas.Ui
import "."

// Weather: condition glyph and temperature from wttr.in, which
// WeatherState fetches (once for every bar's widget, every half hour and on
// demand). A left click opens the forecast panel (the manifest names it); a
// second button re-fetches. Hidden until the first successful fetch.
BarButton {
	id: root

	text: WeatherState.condition !== ""
		? WeatherState.glyph + " " + WeatherState.condition
		: ""

	Component.onCompleted: WeatherState.watch()

	Component.onDestruction: WeatherState.unwatch()

	onPressed: function(button) {
		if (button !== Qt.LeftButton) {
			WeatherState.refresh()
		}
	}
}
