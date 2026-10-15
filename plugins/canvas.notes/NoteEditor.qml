import Quickshell
import Quickshell.Wayland
import QtQuick
import WaylandCanvas.Ui
import "."

// One note editor surface: hidden until a note is activated on this output
// — a double-click, or the create action's item.activate — then a card
// exactly at the activated box, in the note's colour, with the darker
// strip, the colour dots and the × on it, and the text with the cursor at
// its end. It asks for the keyboard on demand, so it holds the keys as it
// maps, in the normal view and the overview alike. Escape or a click
// outside closes it and keeps the text; a note closed with no text is
// deleted; the dots recolour; the × deletes. A drag of the open note
// closes it first, keeping the text.
PanelWindow {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null // the output this editor lives on

	Component.onCompleted: Notes.init(api)

	screen: modelData
	visible: open
	color: "transparent"
	exclusionMode: ExclusionMode.Ignore // an overlay, not a reservation

	WlrLayershell.namespace: "wayland-canvas-note-editor"
	WlrLayershell.layer: WlrLayer.Overlay
	WlrLayershell.keyboardFocus: open ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

	anchors {
		top: true
		bottom: true
		left: true
		right: true
	}

	property bool open: false
	property string noteId: ""
	property color noteColor: "transparent"
	// The card the activated box named, output-local logical px.
	property real boxX: 0
	property real boxY: 0
	property real boxW: 0
	property real boxH: 0
	// The note's canvas size, to scale the strip and the text the way the
	// compositor draws the card itself.
	property real noteWidth: 1
	property real noteHeight: 1

	readonly property real zoom: noteWidth > 0 ? boxW / noteWidth : 1
	// The compositor's card metrics (src/core/config.h), scaled by the zoom
	// the box shows: a 26 canvas-px strip, 10 canvas-px padding, dark ink.
	readonly property real stripH: 26 * zoom
	readonly property real pad: 10 * zoom
	readonly property color ink: "#1f2430"
	// The strip's shade of the card's colour: its ink at 85 percent.
	readonly property color stripColor: Qt.darker(noteColor, 1 / 0.85)
	readonly property real dotSize: Math.max(4, stripH * 0.5)
	readonly property real dotGap: Math.max(2, stripH * 0.15)

	function openNote(id, box) {
		var found = Notes.notes[id]
		if (found == null) {
			return
		}
		noteId = id
		field.text = found.text
		noteColor = found.color
		noteWidth = found.width
		noteHeight = found.height
		boxX = box.x
		boxY = box.y
		boxW = box.width
		boxH = box.height
		open = true
		field.forceActiveFocus()
		field.cursorPosition = field.length // the cursor at the end of the text
	}

	function closeEditor() {
		if (!open) {
			return
		}
		open = false
		Notes.setText(noteId, field.text)
	}

	function deleteNote() {
		if (!open) {
			return
		}
		open = false
		Notes.remove(noteId)
	}

	Connections {
		target: root.api != null ? root.api.compositor : null
		function onEvent(name, params) {
			if (name === "item.activated"
					&& params.output === (root.modelData != null ? root.modelData.name : "")) {
				root.openNote(params.id, params.box)
			} else if (name === "item.moved" && root.open && params.id === root.noteId) {
				// The note was dragged while its editor was open: close
				// first, keeping the text.
				root.closeEditor()
			}
		}
	}

	// The compositor hid the surface behind `open`'s back: resync, and
	// keep the text.
	onVisibleChanged: if (!visible && open) {
		open = false
		Notes.setText(noteId, field.text)
	}

	Item {
		anchors.fill: parent

		// A click anywhere the card is not closes the editor.
		MouseArea {
			anchors.fill: parent
			onClicked: root.closeEditor()
		}

		Rectangle {
			id: card

			x: root.boxX
			y: root.boxY
			width: root.boxW
			height: root.boxH
			color: root.noteColor

			// Swallow clicks on the card itself: only clicks truly outside
			// close the editor.
			MouseArea {
				anchors.fill: parent
			}

			// The darker strip along the top.
			Rectangle {
				id: strip

				width: parent.width
				height: root.stripH
				color: root.stripColor
			}

			// The colour dots: one per settings colour, each a recolour.
			Row {
				x: root.pad
				y: (root.stripH - root.dotSize) / 2
				spacing: root.dotGap

				Repeater {
					model: Notes.dotList

					Rectangle {
						id: dot
						required property var modelData
						width: root.dotSize
						height: root.dotSize
						radius: root.dotSize / 2
						color: modelData.color

						MouseArea {
							anchors.fill: parent
							cursorShape: Qt.PointingHandCursor
							onClicked: {
								root.noteColor = dot.modelData.color
								Notes.recolor(root.noteId, dot.modelData.color)
							}
						}
					}
				}
			}

			// The × at the strip's right: delete.
			Item {
				anchors.right: parent.right
				anchors.top: parent.top
				width: root.stripH
				height: root.stripH

				Text {
					anchors.centerIn: parent
					text: "×"
					color: root.ink
					font {
						family: UiTheme.fontFamily
						pixelSize: Math.max(5, Math.round(root.stripH * 0.66))
					}
					renderType: Text.NativeRendering
				}

				MouseArea {
					anchors.fill: parent
					cursorShape: Qt.PointingHandCursor
					onClicked: root.deleteNote()
				}
			}

			// The text, wrapped to the card as the compositor wraps it.
			TextEdit {
				id: field

				x: root.pad
				y: root.stripH + root.pad
				width: parent.width - 2 * root.pad
				height: parent.height - root.stripH - 2 * root.pad
				color: root.ink
				wrapMode: TextEdit.Wrap
				font {
					family: "Sans"
					pixelSize: Math.max(1, Math.round(10 * root.zoom))
				}
				focus: true
				Keys.priority: Keys.BeforeItem
				Keys.onEscapePressed: root.closeEditor()
				onTextChanged: if (root.open) Notes.typed(root.noteId, text)
			}
		}
	}
}
