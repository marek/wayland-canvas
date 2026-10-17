import QtQuick
import "."

// canvas.notifications/dismiss: the newest popup leaves the screen; the
// history keeps it.
Item {
	property var api: null

	function run(args) {
		Notifications.dismissNewest()
	}
}
