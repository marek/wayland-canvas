import QtQuick
import Quickshell
import Quickshell.Services.Polkit
import Quickshell.Wayland
import WaylandCanvas.Ui
import "."

// The password dialog: polkit asks this shell whenever an application in
// the session wants admin rights, and the answer is typed here
// (docs/intents/shell/password-dialog.md). It holds the keyboard while it
// is open, and nothing else of the shell does.
PanelWindow {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null

	// The seam a scenario drives: without a polkitd on the bus there is no
	// flow to show, so the plugin's stand-in flow stands in for the
	// agent's own.
	readonly property var flow: PolkitState.flow !== null
		? PolkitState.flow : agent.flow
	readonly property bool open: PolkitState.flow !== null
		|| (agent.isActive && agent.flow !== null)

	property string answer: ""

	function submit() {
		if (flow === null || !flow.isResponseRequired) {
			return
		}
		flow.submit(answer)
		answer = ""
	}

	function cancel() {
		if (flow !== null) {
			flow.cancelAuthenticationRequest()
		}
		answer = ""
	}

	PolkitAgent {
		id: agent
		path: "/org/waylandcanvas/PolkitAgent"

		onIsRegisteredChanged: {
			if (isRegistered) {
				console.log("[polkit] the password dialog is registered")
			} else {
				console.warn("[polkit] no password dialog: another agent holds "
					+ "this session")
			}
		}
	}

	visible: open
	color: "transparent"
	exclusionMode: ExclusionMode.Ignore
	screen: modelData

	WlrLayershell.namespace: "wayland-canvas-polkit"
	WlrLayershell.layer: WlrLayer.Overlay
	// The dialog is the one shell surface that takes the keyboard outright:
	// the password must not reach a window behind it.
	WlrLayershell.keyboardFocus: open
		? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

	anchors {
		top: true
		bottom: true
		left: true
		right: true
	}

	// The screen behind is dimmed, and a click on it cancels.
	Rectangle {
		anchors.fill: parent
		color: Qt.rgba(0, 0, 0, 0.45)

		MouseArea {
			anchors.fill: parent
			onClicked: root.cancel()
		}
	}

	Rectangle {
		id: card
		anchors.centerIn: parent
		width: 380
		height: content.implicitHeight + 2 * UiTheme.panelPadding
		radius: 6
		color: UiTheme.panel
		border.width: 1
		border.color: root.flow !== null && root.flow.supplementaryIsError
			? UiTheme.attention : UiTheme.border

		MouseArea {
			anchors.fill: parent // a click on the card is not a cancel
		}

		Column {
			id: content
			focus: true
			Keys.priority: Keys.BeforeItem
			Keys.onEscapePressed: root.cancel()
			x: UiTheme.panelPadding
			y: UiTheme.panelPadding
			width: parent.width - 2 * UiTheme.panelPadding
			spacing: UiTheme.panelSpacing

			Text {
				width: parent.width
				text: "Administrator rights are needed"
				color: UiTheme.text
				font.family: UiTheme.fontFamily
				font.pixelSize: UiTheme.fontTitle
			}

			Text {
				width: parent.width
				text: root.flow !== null ? root.flow.message : ""
				color: UiTheme.text
				wrapMode: Text.WordWrap
				font.family: UiTheme.fontFamily
				font.pixelSize: UiTheme.fontBody
			}

			Text {
				width: parent.width
				visible: text !== ""
				text: root.flow !== null && root.flow.selectedIdentity !== null
					? String(root.flow.selectedIdentity.userName) : ""
				color: UiTheme.muted
				font.family: UiTheme.fontFamily
				font.pixelSize: UiTheme.fontSmall
			}

			PanelField {
				id: field
				width: parent.width
				password: root.flow === null || !root.flow.responseVisible
				enabled: root.flow !== null && root.flow.isResponseRequired
				placeholderText: root.flow !== null && root.flow.inputPrompt !== ""
					? root.flow.inputPrompt : "Password"
				text: root.answer
				onTextChanged: root.answer = text
				onAccepted: root.submit()
			}

			Text {
				width: parent.width
				visible: text !== ""
				text: root.flow !== null ? root.flow.supplementaryMessage : ""
				color: root.flow !== null && root.flow.supplementaryIsError
					? UiTheme.attention : UiTheme.muted
				wrapMode: Text.WordWrap
				font.family: UiTheme.fontFamily
				font.pixelSize: UiTheme.fontSmall
			}

			Row {
				spacing: UiTheme.panelSpacing
				layoutDirection: Qt.RightToLeft
				width: parent.width

				Rectangle {
					width: 90
					height: 24
					radius: 4
					color: UiTheme.accent

					Text {
						anchors.centerIn: parent
						text: "Authenticate"
						color: UiTheme.onAccent
						font.family: UiTheme.fontFamily
						font.pixelSize: UiTheme.fontSmall
					}

					MouseArea {
						anchors.fill: parent
						onClicked: root.submit()
					}
				}

				Rectangle {
					width: 70
					height: 24
					radius: 4
					color: UiTheme.fill

					Text {
						anchors.centerIn: parent
						text: "Cancel"
						color: UiTheme.text
						font.family: UiTheme.fontFamily
						font.pixelSize: UiTheme.fontSmall
					}

					MouseArea {
						anchors.fill: parent
						onClicked: root.cancel()
					}
				}
			}
		}
	}

	onOpenChanged: {
		if (open) {
			answer = ""
			Qt.callLater(function() { field.forceActiveFocus() })
		}
	}
}
