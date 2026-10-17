pragma Singleton

import QtQuick

// What the lock action and the lock surface share: one flag the action
// raises and the surface acts on, so a lock asked for anywhere — the
// shortcut, logind, the idle timeout, all of which reach the shell as this
// plugin's action — reaches the one locker.
Item {
	property bool requested: false

	function lock() {
		requested = true
	}
}
