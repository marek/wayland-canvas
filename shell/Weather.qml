import Quickshell.Io
import QtQuick
import "."

// Weather: condition glyph and temperature from wttr.in, re-fetched every
// half hour and on demand. The click opens the forecast panel; a second
// button re-fetches. Hidden until the first successful fetch.
BarButton {
	id: root

	property string condition: ""
	property string glyph: ""
	property var forecast: []

	text: condition !== "" ? glyph + " " + condition : ""

	Process {
		id: fetch
		command: ["curl", "-fsS", "--max-time", "5", "https://wttr.in/?format=j1"]
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: root.parse(text)
		}
	}

	Timer {
		interval: 1800000
		repeat: true
		running: true
		triggeredOnStart: true
		onTriggered: if (!fetch.running) fetch.running = true
	}

	onPressed: function(button) {
		if (button === Qt.LeftButton) {
			Panels.toggle(panel)
		} else if (!fetch.running) {
			fetch.running = true
		}
	}

	WeatherPanel {
		id: panel
		theme: root.theme
		anchorItem: root
		model: root
	}

	function parse(raw) {
		try {
			var json = JSON.parse(raw)
			var current = json.current_condition[0]
			condition = Math.round(Number(current.temp_C)) + "°C"
			glyph = iconForCode(Number(current.weatherCode), isNight())
			forecast = parseForecast(json, current)
		} catch (e) {
			// A failed fetch keeps the last good reading.
		}
	}

	// wttr.in's `weather` array: today plus the coming days. The glyph is
	// the midday hour's condition, so night never darkens the forecast.
	function parseForecast(json, current) {
		var days = []
		var list = json.weather || []
		for (var i = 0; i < list.length; i++) {
			var mid = list[i].hourly && list[i].hourly.length > 4 ? list[i].hourly[4] : null
			var code = mid != null ? Number(mid.weatherCode) : Number(current.weatherCode)
			var date = new Date(String(list[i].date) + "T12:00:00")
			days.push({
				name: Qt.formatDateTime(date, "ddd d"),
				glyph: iconForCode(code, false),
				min: Math.round(Number(list[i].mintempC)),
				max: Math.round(Number(list[i].maxtempC)),
			})
		}
		return days
	}

	function isNight() {
		var hour = new Date().getHours()
		return hour < 7 || hour >= 19
	}

	// wttr.in condition codes to weather glyphs.
	function iconForCode(code, night) {
		switch (code) {
		case 113:
			return night ? "\ue32b" : "\ue30d"
		case 116:
			return night ? "\ue32e" : "\ue302"
		case 119:
		case 122:
			return "\ue33d"
		case 143:
		case 248:
		case 260:
			return night ? "\ue346" : "\ue313"
		case 176:
		case 263:
		case 353:
			return night ? "\ue333" : "\ue308"
		case 179:
		case 227:
		case 230:
		case 323:
		case 326:
		case 368:
			return night ? "\ue327" : "\ue30a"
		case 182:
		case 185:
		case 281:
		case 284:
		case 311:
		case 314:
		case 317:
		case 320:
		case 350:
		case 362:
		case 365:
		case 374:
		case 377:
			return "\ue3ad"
		case 200:
		case 386:
		case 389:
		case 392:
		case 395:
			return "\ue31d"
		case 266:
		case 293:
		case 296:
		case 299:
		case 302:
		case 305:
		case 308:
		case 356:
		case 359:
			return "\ue318"
		default:
			return "\ue33d"
		}
	}
}
