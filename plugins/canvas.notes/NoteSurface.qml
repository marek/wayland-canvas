import Quickshell
import Quickshell.Wayland
import QtQuick
import QtQuick.Controls
import WaylandCanvas.Ui
import "."

// One note on the plane: a card of its colour with a darker strip on top
// holding its controls — the colour dots, bold, italic and underline, the
// font, the text size between − and +, and the × — the text written
// straight into it, and a grip in the bottom-right corner. The compositor
// places the surface at the note's canvas rectangle and draws nothing of
// it; panning, zooming and a desktop's scale move and size it because
// they move everything on the plane, and a drag of the strip or the grip
// is the compositor's too
// (docs/intents/shell/notes.md).
PanelWindow {
	id: root

	required property string modelData
	property var api: null

	// The note itself: one object, changed in place, whose properties tell
	// this surface what changed.
	readonly property var note: Notes.notes[modelData]
	// The name the compositor places this surface by. It is built here,
	// from the id alone: a layer surface carries the namespace it was
	// created with, so a name that arrives later would never reach the
	// compositor.
	readonly property string surfaceName: "wayland-canvas-canvas-canvas.notes/"
		+ modelData

	readonly property color ink: Notes.ink
	readonly property color paper: note !== undefined ? note.color : "transparent"

	// A note belongs on the plane: one the compositor will not place says
	// so and stays off the screen.
	property bool unplaceable: false
	// A drag of the strip or the grip is running: the compositor sizes and
	// moves the surface until it answers, and nothing here places it.
	property bool dragging: false
	// On demand while it may be clicked into; none for a moment after
	// Escape, so the compositor hands the keyboard back to the window.
	property bool asksForKeyboard: true

	visible: note !== undefined && !unplaceable
	color: "transparent"
	exclusionMode: ExclusionMode.Ignore

	WlrLayershell.namespace: surfaceName
	WlrLayershell.layer: WlrLayer.Overlay
	WlrLayershell.keyboardFocus: asksForKeyboard ? WlrKeyboardFocus.OnDemand
		: WlrKeyboardFocus.None

	// The surface's own size is the note's canvas size; where it goes on
	// the plane is the compositor's to apply.
	implicitWidth: note !== undefined ? note.width : 1
	implicitHeight: note !== undefined ? note.height : 1

	readonly property rect canvasRect: note !== undefined
		? Qt.rect(note.x, note.y, note.width, note.height) : Qt.rect(0, 0, 0, 0)
	onCanvasRectChanged: Qt.callLater(place)

	function place() {
		if (note === undefined || api === null || dragging) {
			return
		}
		api.canvas.place(surfaceName, {
			x: note.x, y: note.y, width: note.width, height: note.height
		}, function(result, error) {
			if (result === true || unplaceable) {
				return
			}
			unplaceable = true
			console.warn("[canvas.notes] the compositor would not place "
				+ surfaceName + ": " + String(error))
		})
	}

	// The narrowest a note gets: its strip's controls side by side.
	function minWidth() {
		return Math.ceil(controls.implicitWidth + closeBox.width + 2 * stripPadding + 2)
	}

	// The height the card needs for its text, strip and padding.
	function neededHeight() {
		return Math.ceil(strip.height + bodyTop + body.contentHeight + bodyBottom)
	}

	// A note grows to hold its text and is never narrower than its strip;
	// it never shrinks by itself.
	function growToFit() {
		if (note === undefined || dragging || body.loading) {
			return
		}
		var width = Math.max(note.width, minWidth())
		var height = Math.max(note.height, neededHeight())
		if (width !== note.width || height !== note.height) {
			Notes.resized(note, width, height)
		}
	}

	// A press on the strip hands the drag to the compositor, which moves
	// the note with the pointer and says where it was dropped.
	function startMove() {
		if (note === undefined || api === null) {
			return
		}
		dragging = true
		api.canvas.move(surfaceName, function(box, error) {
			dragging = false
			if (box != null && typeof box === "object" && note !== undefined) {
				Notes.moved(note, box.x, box.y)
			}
		})
	}

	// Likewise the grip: the compositor sizes the note from its
	// bottom-right corner, never smaller than its strip and its text.
	function startResize() {
		if (note === undefined || api === null) {
			return
		}
		dragging = true
		api.canvas.resize(surfaceName, {
			edges: ["right", "bottom"],
			minWidth: minWidth(),
			minHeight: neededHeight(),
		}, function(box, error) {
			dragging = false
			if (box != null && typeof box === "object" && note !== undefined) {
				Notes.resized(note, box.width, box.height)
			}
			growToFit()
		})
	}

	// Escape: the keyboard goes back to the window; the note asks for it
	// on demand again once it has gone (windowActive below).
	function giveKeyboardBack() {
		fontMenu.close()
		asksForKeyboard = false
	}

	// ---- formatting ----

	// What the strip shows: the format of the selection — bold, italic or
	// underline where all of it has one, its size where it has one size —
	// or the format at the caret. Read from the characters: the caret's own
	// format is the one before it, outside a selection made backwards.
	property var shown: ({bold: false, italic: false, underline: false, size: 0})

	function refreshShown() {
		shown = selectionFormat()
	}

	function selectionFormat() {
		if (body.selectionStart === body.selectionEnd) {
			var caret = body.cursorSelection.font
			return {bold: caret.bold, italic: caret.italic, underline: caret.underline,
				size: sizeOf(caret)}
		}
		var format = {bold: true, italic: true, underline: true, size: -1}
		for (var i = body.selectionStart; i < body.selectionEnd; i++) {
			if (isBreak(i)) {
				continue
			}
			var font = charFont(i)
			format.bold = format.bold && font.bold
			format.italic = format.italic && font.italic
			format.underline = format.underline && font.underline
			var px = sizeOf(font)
			format.size = format.size === -1 || format.size === px ? px : 0
		}
		if (format.size === -1) { // nothing but line breaks
			return {bold: false, italic: false, underline: false,
				size: sizeOf(charFont(body.selectionStart))}
		}
		return format
	}

	function shownFormat(attribute) {
		if (body.pending[attribute] !== undefined) {
			return body.pending[attribute]
		}
		return shown[attribute] === true
	}

	// The text size the size control shows, in pixels: 0 over a selection
	// of several sizes.
	readonly property int shownSize: body.pending.pixelSize !== undefined
		? body.pending.pixelSize : shown.size
	readonly property int smallestSize: 6
	readonly property int largestSize: 200

	function sizeOf(font) {
		return font.pixelSize > 0 ? font.pixelSize : Math.round(font.pointSize * 4 / 3)
	}

	function clampSize(px) {
		return Math.max(smallestSize, Math.min(largestSize, Math.round(px)))
	}

	// One character's format, read through a selection of its own.
	function charFont(position) {
		probe.selectionStart = position // the start first: setting it collapses the range
		probe.selectionEnd = position + 1
		return probe.font
	}

	// A line or paragraph break has no text, so no format of its own.
	function isBreak(position) {
		return /[\n\r\u2028\u2029]/.test(body.getText(position, position + 1))
	}

	function sameFormat(a, b) {
		return a.bold === b.bold && a.italic === b.italic
			&& a.underline === b.underline && sizeOf(a) === sizeOf(b)
	}

	// One attribute changed over the selection, run by run of one format:
	// a whole font set over a mixed span would flatten the weight, slant,
	// underline or size of its other parts. `value` is the new value, or a
	// function of the run's font giving it.
	function applyToSelection(attribute, value) {
		var end = body.selectionEnd
		body.loading = true
		for (var i = body.selectionStart; i < end;) {
			var font = charFont(i)
			var j = i + 1
			while (j < end && sameFormat(charFont(j), font)) {
				j++
			}
			typed.selectionStart = i
			typed.selectionEnd = j
			var changed = typed.font
			changed[attribute] = typeof value === "function" ? value(font) : value
			typed.font = changed
			i = j
		}
		body.loading = false
		Notes.typed(root.note, Notes.cleanHtml(body.text))
		refreshShown()
	}

	// Something chosen on the strip: on the selected span, or — with
	// nothing selected — on what is typed next.
	function applyFormat(attribute, value) {
		if (body.selectionStart !== body.selectionEnd) {
			applyToSelection(attribute, value)
			return
		}
		var next = Object.assign({}, body.pending)
		next[attribute] = value
		body.pending = next
	}

	// Bold, italic or underline: off when all of the selection has it.
	function toggleFormat(attribute) {
		if (body.selectionStart !== body.selectionEnd) {
			applyToSelection(attribute, !selectionFormat()[attribute])
			return
		}
		applyFormat(attribute, !shownFormat(attribute))
	}

	// A size typed into the strip: all of the selection takes it.
	function applySize(px) {
		applyFormat("pixelSize", clampSize(px))
	}

	// − and +: a pixel smaller or larger, each part of a selection from its
	// own size.
	function stepSize(delta) {
		if (body.selectionStart !== body.selectionEnd) {
			applyToSelection("pixelSize", function(font) {
				return clampSize(sizeOf(font) + delta)
			})
			return
		}
		applyFormat("pixelSize", clampSize(shownSize + delta))
	}

	// The note's font changed: the text is read back from the note, so no
	// span keeps the one it was typed in.
	function reloadText() {
		var caret = body.cursorPosition
		body.loading = true
		body.text = Notes.richHtml(note.html)
		body.lastLength = body.length
		body.loading = false
		body.cursorPosition = Math.min(caret, body.length)
	}

	readonly property string noteFont: note !== undefined ? note.font : ""
	// The size of text that names none: the note's own.
	readonly property int noteSize: note !== undefined ? note.size : 0
	onNoteFontChanged: if (!body.loading) Qt.callLater(reloadText)

	readonly property int stripPadding: 6
	readonly property int bodyTop: 4
	readonly property int bodyBottom: 16 // the grip's corner stays clear

	Component.onCompleted: {
		Qt.callLater(place)
		if (note !== undefined) {
			body.text = Notes.richHtml(note.html)
		}
		body.lastLength = body.length
		body.loading = false
		body.cursorPosition = body.length
		Qt.callLater(function() {
			body.forceActiveFocus()
			growToFit()
		})
	}

	component StripButton: Rectangle {
		id: button

		property string label: ""
		property bool active: false
		property bool bold: false
		property bool italic: false
		property bool underline: false
		property string family: ""
		signal clicked()

		width: Math.max(14, caption.implicitWidth + 6)
		height: 16
		radius: 3
		color: active ? Qt.darker(root.paper, 1.45)
			: (area.containsMouse ? Qt.darker(root.paper, 1.3) : "transparent")

		Text {
			id: caption
			anchors.centerIn: parent
			text: button.label
			color: root.ink
			font.pixelSize: 11
			font.bold: button.bold
			font.italic: button.italic
			font.underline: button.underline
			font.family: button.family !== "" ? button.family : UiTheme.fontFamily
		}

		MouseArea {
			id: area
			anchors.fill: parent
			anchors.margins: -1 // a small target at any zoom, clear of its neighbours
			hoverEnabled: true
			onClicked: button.clicked()
			// Two quick clicks are two clicks: + twice is two pixels.
			onDoubleClicked: button.clicked()
		}
	}

	Rectangle {
		id: card
		anchors.fill: parent
		color: root.paper

		// The keyboard went (Escape, a click elsewhere): the note asks for
		// it on demand again, and one left empty is deleted.
		readonly property bool windowActive: Window.active
		onWindowActiveChanged: {
			if (windowActive) {
				return
			}
			root.asksForKeyboard = true
			if (root.note !== undefined && body.length === 0) {
				var note = root.note
				Qt.callLater(function() { Notes.remove(note) })
			}
		}

		// The strip: the note's own controls, and the handle it is dragged by.
		Rectangle {
			id: strip
			width: parent.width
			height: 26
			color: Qt.darker(root.paper, 1.18)

			// Behind the controls: a press here hands the drag to the
			// compositor.
			MouseArea {
				anchors.fill: parent
				onPressed: root.startMove()
			}

			Row {
				id: controls
				anchors.left: parent.left
				anchors.leftMargin: root.stripPadding
				anchors.verticalCenter: parent.verticalCenter
				spacing: 4

				Row {
					anchors.verticalCenter: parent.verticalCenter
					spacing: 3

					Repeater {
						model: Notes.dotList

						delegate: Rectangle {
							id: dot

							required property var modelData

							width: 8
							height: 8
							radius: 4
							color: modelData.color
							border.width: Qt.colorEqual(root.paper, modelData.color) ? 2 : 0
							border.color: root.ink

							MouseArea {
								anchors.fill: parent
								anchors.margins: -1 // a dot is a small target, its neighbours close
								hoverEnabled: true
								onClicked: Notes.recolour(root.note, dot.modelData.color)
							}
						}
					}
				}

				Row {
					anchors.verticalCenter: parent.verticalCenter
					spacing: 1

					StripButton {
						objectName: "bold"
						label: "B"
						bold: true
						active: root.shownFormat("bold")
						onClicked: root.toggleFormat("bold")
					}
					StripButton {
						objectName: "italic"
						label: "I"
						italic: true
						active: root.shownFormat("italic")
						onClicked: root.toggleFormat("italic")
					}
					StripButton {
						objectName: "underline"
						label: "U"
						underline: true
						active: root.shownFormat("underline")
						onClicked: root.toggleFormat("underline")
					}
				}

				StripButton {
					anchors.verticalCenter: parent.verticalCenter
					objectName: "font"
					label: "Aa"
					family: root.noteFont
					active: fontMenu.opened
					onClicked: fontMenu.opened ? fontMenu.close() : fontMenu.open()
				}

				// The text size: − and + step it, the number can be typed.
				Row {
					anchors.verticalCenter: parent.verticalCenter

					StripButton {
						objectName: "smaller"
						width: 12
						label: "\u2212"
						onClicked: root.stepSize(-1)
					}
					Rectangle {
						width: 20
						height: 16
						radius: 3
						color: sizeField.activeFocus ? Qt.lighter(root.paper, 1.15) : "transparent"

						TextInput {
							id: sizeField
							objectName: "size"
							anchors.fill: parent
							horizontalAlignment: TextInput.AlignHCenter
							verticalAlignment: TextInput.AlignVCenter
							color: root.ink
							selectionColor: root.ink
							selectedTextColor: root.paper
							font.pixelSize: 11
							font.family: UiTheme.fontFamily
							maximumLength: 3
							validator: IntValidator {
								bottom: root.smallestSize
								top: root.largestSize
							}
							text: root.shownSize > 0 ? String(root.shownSize) : ""
							// Clicked into, the number is ready to be typed over.
							onActiveFocusChanged: {
								if (activeFocus) {
									selectAll()
									return
								}
								text = Qt.binding(function() {
									return root.shownSize > 0 ? String(root.shownSize) : ""
								})
							}
							onAccepted: {
								root.applySize(Number(text))
								body.forceActiveFocus()
							}
							Keys.onEscapePressed: body.forceActiveFocus()
						}
					}
					StripButton {
						objectName: "larger"
						width: 12
						label: "+"
						onClicked: root.stepSize(1)
					}
				}
			}

			// The × and the square around it: a glyph is a smaller target
			// than a finger or a pointer at any zoom.
			Rectangle {
				id: closeBox
				anchors.right: parent.right
				anchors.verticalCenter: parent.verticalCenter
				width: 20
				height: parent.height
				// The close square lights up under the pointer and darkens
				// as it is pressed, as a window's own close button does.
				color: closeArea.pressed ? "#c42b1c"
					: (closeArea.containsMouse ? "#e81123" : "transparent")

				Text {
					anchors.centerIn: parent
					text: "\u00d7"
					color: closeArea.containsMouse ? "#ffffff" : root.ink
				}

				MouseArea {
					id: closeArea
					anchors.fill: parent
					hoverEnabled: true
					// later: the surface goes with the note, and this handler with it
					onClicked: {
						var note = root.note
						Qt.callLater(function() { Notes.remove(note) })
					}
				}
			}
		}

		TextArea {
			id: body

			// Set while the text is written in from the note, not typed.
			property bool loading: true
			// The format chosen with nothing selected, for what is typed
			// next: attribute -> on.
			property var pending: ({})
			property int lastLength: 0

			anchors.fill: parent
			anchors.topMargin: strip.height + root.bodyTop
			anchors.leftMargin: 8
			anchors.rightMargin: 8
			anchors.bottomMargin: root.bodyBottom
			padding: 0
			focus: true
			activeFocusOnPress: true
			persistentSelection: true
			textFormat: TextEdit.RichText
			wrapMode: TextEdit.Wrap
			color: root.ink
			selectionColor: root.ink
			selectedTextColor: root.paper
			background: null
			font.family: root.noteFont !== "" ? root.noteFont : UiTheme.fontFamily
			font.pixelSize: root.noteSize > 0 ? root.noteSize : UiTheme.fontBody

			onTextChanged: {
				Qt.callLater(root.refreshShown) // formatting changes the text too
				if (loading || root.note === undefined) {
					return
				}
				var grown = length - lastLength
				lastLength = length
				applyPending(grown)
				Notes.typed(root.note, Notes.cleanHtml(body.text))
			}
			onContentHeightChanged: root.growToFit()
			onSelectionStartChanged: Qt.callLater(root.refreshShown)
			onSelectionEndChanged: Qt.callLater(root.refreshShown)
			// A caret moved by hand drops a choice made for what is typed next.
			onCursorPositionChanged: {
				Qt.callLater(root.refreshShown)
				if (length === lastLength && Object.keys(pending).length > 0) {
					pending = ({})
				}
			}

			// What was just typed takes the format chosen for it; typing
			// on continues the span it began.
			function applyPending(grown) {
				var chosen = pending
				if (grown <= 0 || Object.keys(chosen).length === 0) {
					return
				}
				pending = ({})
				var end = cursorPosition
				if (end - grown < 0) {
					return
				}
				typed.selectionStart = end - grown
				typed.selectionEnd = end
				var font = typed.font
				for (var attribute in chosen) {
					font[attribute] = chosen[attribute]
				}
				loading = true
				typed.font = font
				loading = false
			}

			Keys.onPressed: function(event) {
				if (event.key === Qt.Key_Escape) {
					root.giveKeyboardBack()
					event.accepted = true
					return
				}
				if ((event.modifiers & Qt.ControlModifier) === 0) {
					return
				}
				var attribute = event.key === Qt.Key_B ? "bold"
					: event.key === Qt.Key_I ? "italic"
					: event.key === Qt.Key_U ? "underline" : ""
				if (attribute !== "") {
					root.toggleFormat(attribute)
					event.accepted = true
				} else if (event.key === Qt.Key_V) {
					// pasted formatting of its own goes once it has landed
					Qt.callLater(function() {
						Notes.typed(root.note, Notes.cleanHtml(body.text))
						root.reloadText()
					})
				}
			}

			// A click in the card puts the caret in it, as a click in a
			// window does.
			MouseArea {
				anchors.fill: parent
				acceptedButtons: Qt.LeftButton
				cursorShape: Qt.IBeamCursor
				onPressed: function(mouse) {
					body.forceActiveFocus()
					mouse.accepted = false
				}
			}
		}

		// The span just typed, for the format chosen before it was.
		TextSelection {
			id: typed
			document: body.textDocument
		}

		// One character at a time, read for the strip; never written.
		TextSelection {
			id: probe
			document: body.textDocument
		}

		// The grip: a press hands the corner to the compositor, which sizes
		// the note with the pointer at any zoom.
		MouseArea {
			id: grip
			anchors.right: parent.right
			anchors.bottom: parent.bottom
			width: 16
			height: 16
			cursorShape: Qt.SizeFDiagCursor
			onPressed: root.startResize()

			Canvas {
				anchors.fill: parent
				onPaint: {
					var ctx = getContext("2d")
					ctx.reset()
					ctx.strokeStyle = root.ink
					ctx.globalAlpha = 0.45
					for (var i = 1; i <= 3; i++) {
						ctx.beginPath()
						ctx.moveTo(width - i * 4, height)
						ctx.lineTo(width, height - i * 4)
						ctx.stroke()
					}
				}
			}
		}

		// The font: every family installed on the machine, narrowed by what
		// is typed into the filter.
		Popup {
			id: fontMenu
			x: 4
			y: strip.height
			width: card.width - 8
			height: Math.max(80, card.height - strip.height - 6)
			padding: 4
			focus: true
			closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

			property var families: []

			function pickFirst() {
				if (fontList.count > 0) {
					Notes.setFont(root.note, fontList.model[0])
					close()
				}
			}

			onAboutToShow: {
				families = Qt.fontFamilies()
				filter.text = ""
			}
			onOpened: filter.forceActiveFocus()
			onClosed: body.forceActiveFocus()

			background: Rectangle {
				color: Qt.lighter(root.paper, 1.08)
				border.color: Qt.darker(root.paper, 1.4)
				radius: 4
			}

			contentItem: Column {
				spacing: 4

				TextField {
					id: filter
					width: parent.width
					placeholderText: "Font"
					color: root.ink
					font.pixelSize: 12
					background: Rectangle {
						color: Qt.lighter(root.paper, 1.18)
						radius: 3
					}
					Keys.onEscapePressed: fontMenu.close()
					// Enter takes the first family the filter leaves.
					Keys.onReturnPressed: fontMenu.pickFirst()
					Keys.onEnterPressed: fontMenu.pickFirst()
				}

				ListView {
					id: fontList
					width: parent.width
					height: fontMenu.availableHeight - filter.height - 4
					clip: true
					boundsBehavior: Flickable.StopAtBounds
					model: fontMenu.families.filter(function(family) {
						return family.toLowerCase().indexOf(filter.text.toLowerCase()) >= 0
					})

					delegate: Rectangle {
						id: row

						required property string modelData

						width: fontList.width
						height: 20
						color: rowArea.containsMouse ? Qt.darker(root.paper, 1.15)
							: (modelData === root.noteFont ? Qt.darker(root.paper, 1.08)
								: "transparent")

						Text {
							anchors.left: parent.left
							anchors.leftMargin: 4
							anchors.right: parent.right
							anchors.verticalCenter: parent.verticalCenter
							text: row.modelData
							elide: Text.ElideRight
							color: root.ink
							font.family: row.modelData
							font.pixelSize: 12
						}

						MouseArea {
							id: rowArea
							anchors.fill: parent
							hoverEnabled: true
							onClicked: {
								Notes.setFont(root.note, row.modelData)
								fontMenu.close()
							}
						}
					}
				}
			}
		}
	}
}
