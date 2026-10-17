import QtQuick
import WaylandCanvas.Ui
import "."

// The bell: a plain bell while nothing waits, a ringing one with the
// history's count beside it, a struck-through one while popups are
// silenced. A click opens the history panel the manifest names, so the
// bar's own `api` and `panel` must reach BarButton untouched.
BarButton {
	readonly property int waiting: Notifications.history.length

	Component.onCompleted: Notifications.init(api)

	// A count needs room the fixed icon slot does not have.
	icon: waiting === 0 || Notifications.silent
	text: Notifications.silent ? "\u{f009b}"
		: (waiting > 0 ? "\u{f009a} " + waiting : "\u{f009a}")
}
