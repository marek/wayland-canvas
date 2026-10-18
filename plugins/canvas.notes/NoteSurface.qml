import Quickshell
import Quickshell.Wayland
import QtQuick
import QtQuick.Controls
import WaylandCanvas.Ui
import "."

// One note on the plane: a card of its colour with a darker strip on top,
// the text written straight into it, the colour dots and the × on the
// strip. The compositor places the surface at the note's canvas rectangle
// and draws nothing of it; panning, zooming and a desktop's scale move
// and size it because they move everything on the plane.
PanelWindow {
	id: root

	property var api: null
	property string noteId: ""

	readonly property var note: Notes.notes[noteId]
	readonly property string surfaceName: api !== null
		? api.canvas.surfaceName("canvas.notes", noteId) : ""

	visible: note !== undefined && !unplaceable
	color: "transparent"
	exclusionMode: ExclusionMode.Ignore

	WlrLayershell.namespace: surfaceName
	WlrLayershell.layer: WlrLayer.Overlay
	// A note takes the keyboard when it is typed in, like any shell
	// surface that asks on demand; it never holds it against a window.
	WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

	// The surface's own size is the note's canvas size; where it goes on
	// the plane is the compositor's to apply.
	implicitWidth: note !== undefined ? note.width : 1
	implicitHeight: note !== undefined ? note.height : 1

	// The compositor can only place a surface it knows, and it learns of
	// one when the client creates it: the ask is repeated until it lands.
	property bool placed: false
	// A note belongs on the plane: one that cannot be put there says so
	// and stays off the screen, rather than floating over the middle of
	// it where it would cover what is behind and do nothing.
	property int placeTries: 0
	readonly property bool unplaceable: placeTries > 40 && !placed

	function place() {
		if (note === undefined || api === null) {
			return
		}
		api.canvas.place(surfaceName, {
			x: note.x, y: note.y, width: note.width, height: note.height
		}, function(result, error) {
			placed = result === true
			if (!placed) {
				placeTries++
				if (placeTries === 41) {
					console.warn("[canvas.notes] the compositor would not place "
						+ surfaceName + ": " + error)
				}
			}
		})
	}

	Timer {
		interval: 150
		repeat: true
		running: !root.placed && !root.unplaceable && root.note !== undefined
		onTriggered: root.place()
	}

	Component.onCompleted: {
		Qt.callLater(place)
		Qt.callLater(function() { body.forceActiveFocus() })
	}
	onNoteChanged: Qt.callLater(place)

	Rectangle {
		anchors.fill: parent
		color: root.note !== undefined ? root.note.color : "transparent"

		// The strip: the colour dots and the ×, the note's own controls.
		Rectangle {
			id: strip
			width: parent.width
			height: Math.min(26, parent.height / 4)
			color: Qt.darker(parent.color, 1.18)

			// Dragging the strip moves the note on the plane.
			MouseArea {
				anchors.fill: parent
				hoverEnabled: true
				cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
				propagateComposedEvents: true
				property real pressX: 0
				property real pressY: 0
				onPressed: function(mouse) {
					pressX = mouse.x
					pressY = mouse.y
				}
				onPositionChanged: function(mouse) {
					if (!pressed || root.note === undefined) {
						return
					}
					Notes.moved(root.noteId, root.note.x + mouse.x - pressX,
						root.note.y + mouse.y - pressY)
				}
			}
			Row {
				anchors.left: parent.left
				anchors.leftMargin: 6
				anchors.verticalCenter: parent.verticalCenter
				spacing: 6

				Repeater {
					model: Notes.dotList

					delegate: Rectangle {
						id: dot

						required property var modelData

						width: dotArea.containsMouse ? 14 : 10
						height: width
						radius: width / 2
						color: modelData.color
						border.width: root.note !== undefined
							&& root.note.color === modelData.color ? 2 : 0
						border.color: "#1f2430"

						Behavior on width { NumberAnimation { duration: 80 } }

						MouseArea {
							id: dotArea
							anchors.fill: parent
							anchors.margins: -3 // a dot is a small target
							hoverEnabled: true
							cursorShape: Qt.PointingHandCursor
							onClicked: Notes.recolour(root.noteId, dot.modelData.color)
						}
					}
				}
			}

			// The × and the square around it: a glyph is a smaller target
			// than a finger or a pointer at any zoom.
			Rectangle {
				anchors.right: parent.right
				anchors.verticalCenter: parent.verticalCenter
				width: 22
				height: parent.height
				// The close square lights up under the pointer and darkens
				// as it is pressed, as a window's own close button does.
				color: closeArea.pressed ? "#c42b1c"
					: (closeArea.containsMouse ? "#e81123" : "transparent")

				Text {
					anchors.centerIn: parent
					text: "\u00d7"
					color: closeArea.containsMouse ? "#ffffff" : "#1f2430"
				}

				MouseArea {
					id: closeArea
					anchors.fill: parent
					hoverEnabled: true
					cursorShape: Qt.PointingHandCursor
					onClicked: Notes.remove(root.noteId)
				}
			}

		}

		TextArea {
			id: body
			focus: true
			activeFocusOnPress: true
			anchors.fill: parent
			anchors.topMargin: strip.height + 4
			anchors.margins: 8
			wrapMode: TextArea.Wrap
			color: "#1f2430"
			selectionColor: "#1f2430"
			selectedTextColor: root.note !== undefined ? root.note.color : "white"
			background: null
			font.family: UiTheme.fontFamily
			font.pixelSize: UiTheme.fontBody
			text: root.note !== undefined ? root.note.text : ""
			// `body.text` by name: an unqualified `text` here would be the
			// item, not what was typed into it.
			onTextChanged: Notes.typed(root.noteId, body.text)

			// A click in the card puts the caret in it, as a click in a
			// window does.
			MouseArea {
				anchors.fill: parent
				acceptedButtons: Qt.LeftButton
				onClicked: function(mouse) {
					body.forceActiveFocus()
					mouse.accepted = false
				}
			}
		}
	}
}
