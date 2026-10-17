import Quickshell
import Quickshell.Services.Notifications
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui
import "."

// The notification server and its popups: a column down the top-right of
// every output, newest first, each a card with the sender, the summary,
// the body, its image and its action buttons. A popup never takes the
// keyboard (docs/intents/shell/notifications.md).
PanelWindow {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null

	Component.onCompleted: Notifications.init(api)

	screen: modelData
	visible: Notifications.popups.length > 0
	color: "transparent"
	exclusionMode: ExclusionMode.Ignore

	WlrLayershell.namespace: "wayland-canvas-notifications"
	WlrLayershell.layer: WlrLayer.Overlay
	WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

	anchors {
		top: true
		bottom: true
		left: true
		right: true
	}

	// Only the cards take clicks; everything else on the output stays the
	// windows' and the canvas's.
	mask: Region {
		item: column
	}

	// The server lives with the popups: one per shell, named by the first
	// surface to come up.
	NotificationServer {
		id: server
		keepOnReload: false
		imageSupported: true
		actionsSupported: true
		bodyMarkupSupported: true
		persistenceSupported: true

		onNotification: function(notification) {
			notification.tracked = true
			Notifications.add(notification)
		}
	}

	Column {
		id: column
		x: parent.width - width - UiTheme.edgeMargin
		y: UiTheme.barHeight + UiTheme.edgeMargin
		width: 340
		spacing: UiTheme.panelSpacing

		Repeater {
			model: Notifications.popups

			delegate: Rectangle {
				id: card

				required property var modelData
				required property int index

				width: column.width
				height: body.implicitHeight + 2 * UiTheme.panelPadding
				radius: 6
				color: UiTheme.panel
				border.width: 1
				border.color: modelData.urgency === "critical"
					? UiTheme.attention : UiTheme.border

				// A popup of its own age: the sender's timeout, the
				// setting, or never for a critical one.
				Timer {
					interval: Math.max(1, Notifications.lifetime(card.modelData, 0))
					running: Notifications.lifetime(card.modelData, 0) > 0
					repeat: false
					onTriggered: Notifications.dismiss(card.modelData.id)
				}

				MouseArea {
					anchors.fill: parent
					acceptedButtons: Qt.LeftButton | Qt.RightButton
					onClicked: Notifications.dismiss(card.modelData.id)
				}

				Column {
					id: body
					x: UiTheme.panelPadding
					y: UiTheme.panelPadding
					width: parent.width - 2 * UiTheme.panelPadding
					spacing: 4

					Text {
						width: parent.width
						text: card.modelData.appName
						color: UiTheme.muted
						elide: Text.ElideRight
						font.family: UiTheme.fontFamily
						font.pixelSize: UiTheme.fontSmall
					}

					Row {
						width: parent.width
						spacing: UiTheme.panelSpacing

						Image {
							visible: card.modelData.image !== ""
							source: card.modelData.image
							width: visible ? 32 : 0
							height: visible ? 32 : 0
							fillMode: Image.PreserveAspectFit
						}

						Column {
							width: parent.width - (card.modelData.image !== ""
								? 32 + UiTheme.panelSpacing : 0)
							spacing: 2

							Text {
								width: parent.width
								text: card.modelData.summary
								color: UiTheme.text
								wrapMode: Text.WordWrap
								font.family: UiTheme.fontFamily
								font.pixelSize: UiTheme.fontBody
								font.bold: true
							}

							Text {
								width: parent.width
								visible: card.modelData.body !== ""
								text: card.modelData.body
								color: UiTheme.text
								wrapMode: Text.WordWrap
								textFormat: Text.StyledText
								font.family: UiTheme.fontFamily
								font.pixelSize: UiTheme.fontBody
							}
						}
					}

					Row {
						spacing: UiTheme.panelSpacing
						visible: card.modelData.actions.length > 0

						Repeater {
							model: card.modelData.actions

							delegate: Rectangle {
								required property var modelData

								height: 22
								width: label.implicitWidth + 2 * UiTheme.panelPadding
								radius: 4
								color: action.containsMouse ? UiTheme.hover : UiTheme.fill

								Text {
									id: label
									anchors.centerIn: parent
									text: parent.modelData.text
									color: UiTheme.text
									font.family: UiTheme.fontFamily
									font.pixelSize: UiTheme.fontSmall
								}

								MouseArea {
									id: action
									anchors.fill: parent
									hoverEnabled: true
									onClicked: Notifications.invoke(card.modelData.id,
										parent.modelData.id)
								}
							}
						}
					}
				}
			}
		}
	}
}
