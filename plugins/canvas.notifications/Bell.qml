import QtQuick
import WaylandCanvas.Ui
import "."

// The bell: a plain bell while nothing waits, a ringing one with the
// history's count beside it, a struck-through one while popups are
// silenced. A click opens the history panel the manifest names.
BarButton {
	property var api: null

	Component.onCompleted: Notifications.init(api)

	icon: true
	text: Notifications.silent ? "\u{f009b}"
		: (Notifications.history.length > 0
			? "\u{f009a} " + Notifications.history.length : "\u{f009a}")
}
