import QtQuick
import "."

// canvas.lock/lock: the action every lock trigger runs — the `lock`
// shortcut, `loginctl lock-session`, the idle timeout and the sleep the
// compositor holds back until the lock is drawn.
Item {
	property var api: null

	function run(args) {
		LockState.lock()
	}
}
