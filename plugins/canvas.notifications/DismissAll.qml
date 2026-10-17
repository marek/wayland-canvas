import QtQuick
import "."

// canvas.notifications/dismiss-all: every popup leaves the screen at once.
Item {
	property var api: null

	function run(args) {
		Notifications.dismissAll()
	}
}
