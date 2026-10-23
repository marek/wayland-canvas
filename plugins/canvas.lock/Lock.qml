import QtQuick
import Quickshell
import Quickshell.Services.Pam
import Quickshell.Wayland
import WaylandCanvas.Ui
import "."

// The locker: one session lock over every output, the clock and a password
// field on each, and PAM as the only way out. The compositor draws nothing
// behind it (docs/intents/shell/lock.md); a locker that dies leaves the
// session locked, and the shell that comes back takes it over.
Item {
	id: root

	property var api: null

	readonly property string pamService: api !== null
		? api.plugin.settings.pamService : "wayland-canvas"
	readonly property string pamConfigDirectory: api !== null
		? api.plugin.settings.pamConfigDirectory : "/etc/pam.d"

	property string password: ""
	property string failure: ""
	property bool checking: false

	function submit() {
		if (checking || password.length === 0) {
			return
		}
		failure = ""
		checking = true
		if (!pam.start()) {
			checking = false
			failure = "The password cannot be checked"
		}
	}

	function answerPam() {
		if (checking && pam.active && pam.responseRequired) {
			pam.respond(password)
		}
	}

	Connections {
		target: LockState
		function onRequestedChanged() {
			if (LockState.requested && !lock.locked) {
				root.password = ""
				root.failure = ""
				lock.locked = true
			}
		}
	}

	PamContext {
		id: pam
		config: root.pamService
		configDirectory: root.pamConfigDirectory
		user: Quickshell.env("USER") || Quickshell.env("LOGNAME")

		onResponseRequiredChanged: root.answerPam()
		onPamMessage: root.answerPam()

		onCompleted: function(result) {
			root.checking = false
			if (result === PamResult.Success) {
				root.password = ""
				root.failure = ""
				LockState.requested = false
				lock.locked = false
				return
			}
			root.password = ""
			root.failure = "Wrong password"
		}

		onError: function(error) {
			root.checking = false
			root.password = ""
			root.failure = "The password cannot be checked"
		}
	}

	WlSessionLock {
		id: lock

		locked: false

		WlSessionLockSurface {
			color: UiTheme.background

			Column {
				anchors.centerIn: parent
				spacing: 18

				Text {
					anchors.horizontalCenter: parent.horizontalCenter
					text: Qt.formatTime(clock.date, "HH:mm")
					color: UiTheme.text
					font.family: UiTheme.fontFamily
					font.pixelSize: 64
				}

				Text {
					anchors.horizontalCenter: parent.horizontalCenter
					text: Qt.formatDate(clock.date, "dddd d MMMM")
					color: UiTheme.muted
					font.family: UiTheme.fontFamily
					font.pixelSize: UiTheme.fontBody
				}

				PanelField {
					id: field
					width: 320
					password: true
					enabled: !root.checking
					placeholderText: root.checking ? "Checking…" : "Password"
					text: root.password
					onTextChanged: root.password = text
					onAccepted: root.submit()
					Component.onCompleted: forceActiveFocus()
				}

				Text {
					anchors.horizontalCenter: parent.horizontalCenter
					text: root.failure
					color: UiTheme.attention
					font.family: UiTheme.fontFamily
					font.pixelSize: UiTheme.fontBody
				}
			}

			// A key anywhere on the lock surface types into the field.
			Connections {
				target: lock
				function onLockedChanged() {
					if (lock.locked) {
						field.forceActiveFocus()
					}
				}
			}
		}
	}

	// Quickshell's own clock, as the bar's: a QML Timer here never fired
	// while the session was locked, and the time stood still.
	SystemClock {
		id: clock
		precision: SystemClock.Minutes
	}
}
