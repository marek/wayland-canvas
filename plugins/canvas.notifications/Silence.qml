import QtQuick
import "."

// canvas.notifications/silence: turns popups off or on. `args.on` sets it
// outright; without it the switch flips.
Item {
	property var api: null

	function run(args) {
		var on = args != null && args.on !== undefined
			? args.on === true : !Notifications.silent
		Notifications.setSilent(on)
	}
}
