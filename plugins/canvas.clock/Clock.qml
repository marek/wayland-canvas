import Quickshell
import QtQuick
import WaylandCanvas.Ui

// The clock: `dddd HH:mm`; right-click cycles to the ISO-week date form. A
// left click opens the calendar panel (the manifest names it, so BarButton
// opens it without code of its own).
BarButton {
	id: root

	property bool alt: false
	property date now: clock.date

	SystemClock {
		id: clock
		precision: SystemClock.Minutes
	}

	text: {
		var format = root.alt ? "d MMMM 'W'ww yyyy" : "dddd HH:mm"
		return Qt.formatDateTime(root.now, format.replace(/ww/g, isoWeek(root.now)))
	}

	onPressed: function(button) {
		if (button === Qt.RightButton) {
			root.alt = !root.alt
		}
	}

	// ISO-8601 week number, the `ww` Qt.formatDateTime does not know.
	function isoWeek(date) {
		var d = new Date(Date.UTC(date.getFullYear(), date.getMonth(), date.getDate()))
		var day = d.getUTCDay() || 7
		d.setUTCDate(d.getUTCDate() + 4 - day)
		var yearStart = new Date(Date.UTC(d.getUTCFullYear(), 0, 1))
		var week = Math.ceil((((d - yearStart) / 86400000) + 1) / 7)
		return week < 10 ? "0" + week : "" + week
	}
}
