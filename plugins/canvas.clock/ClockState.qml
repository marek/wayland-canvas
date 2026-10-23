pragma Singleton

import Quickshell
import QtQuick

// The time the clock widget on every bar and the calendar panel share,
// to the minute.
Item {
	readonly property date now: clock.date

	SystemClock {
		id: clock
		precision: SystemClock.Minutes
	}
}
