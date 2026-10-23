import QtQuick
import QtQuick.Controls
import WaylandCanvas.Ui
import "noteFile.js" as NoteFile
import "."

// The text of one note, written in place: a rich-text area that keeps the
// note's HTML up to date as it is typed into, and formats it — bold,
// italic, underline and size — on a selection or on what is typed next.
// The strip's controls drive the formatting through the functions below.
TextArea {
	id: root

	// The Note this edits; undefined once it has been removed.
	property var note: undefined
	// The colour of the card behind the text.
	property color paper: "transparent"

	// Escape: the surface gives the keyboard back.
	signal escaped()

	// Set while the text is written in from the note, not typed.
	property bool loading: true
	// The format chosen with nothing selected, for what is typed
	// next: attribute -> on.
	property var pending: ({})
	property int lastLength: 0

	readonly property string noteFont: note !== undefined ? note.font : ""
	// The size of text that names none: the note's own.
	readonly property int noteSize: note !== undefined ? note.size : 0

	// What the strip shows: the format of the selection — bold, italic or
	// underline where all of it has one, its size where it has one size —
	// or the format at the caret. Read from the characters: the caret's own
	// format is the one before it, outside a selection made backwards.
	property var shown: ({bold: false, italic: false, underline: false, size: 0})

	// The text size the size control shows, in pixels: 0 over a selection
	// of several sizes.
	readonly property int shownSize: pending.pixelSize !== undefined
		? pending.pixelSize : shown.size
	readonly property int smallestSize: 6
	readonly property int largestSize: 200

	padding: 0
	focus: true
	activeFocusOnPress: true
	persistentSelection: true
	textFormat: TextEdit.RichText
	wrapMode: TextEdit.Wrap
	color: Notes.ink
	selectionColor: Notes.ink
	selectedTextColor: paper
	background: null
	font.family: noteFont !== "" ? noteFont : UiTheme.fontFamily
	font.pixelSize: noteSize > 0 ? noteSize : UiTheme.fontBody

	function refreshShown() {
		shown = selectionFormat()
	}

	function selectionFormat() {
		if (selectionStart === selectionEnd) {
			var caret = cursorSelection.font
			return {bold: caret.bold, italic: caret.italic, underline: caret.underline,
				size: sizeOf(caret)}
		}
		var format = {bold: true, italic: true, underline: true, size: -1}
		for (var i = selectionStart; i < selectionEnd; i++) {
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
				size: sizeOf(charFont(selectionStart))}
		}
		return format
	}

	function shownFormat(attribute) {
		if (pending[attribute] !== undefined) {
			return pending[attribute]
		}
		return shown[attribute] === true
	}

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
		return /[\n\r\u2028\u2029]/.test(getText(position, position + 1))
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
		var end = selectionEnd
		loading = true
		for (var i = selectionStart; i < end;) {
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
		loading = false
		Notes.typed(note, NoteFile.cleanHtml(text))
		refreshShown()
	}

	// Something chosen on the strip: on the selected span, or — with
	// nothing selected — on what is typed next.
	function applyFormat(attribute, value) {
		if (selectionStart !== selectionEnd) {
			applyToSelection(attribute, value)
			return
		}
		var next = Object.assign({}, pending)
		next[attribute] = value
		pending = next
	}

	// Bold, italic or underline: off when all of the selection has it.
	function toggleFormat(attribute) {
		if (selectionStart !== selectionEnd) {
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
		if (selectionStart !== selectionEnd) {
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
		var caret = cursorPosition
		loading = true
		text = NoteFile.richHtml(note.html)
		lastLength = length
		loading = false
		cursorPosition = Math.min(caret, length)
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

	onNoteFontChanged: if (!loading) Qt.callLater(reloadText)

	Component.onCompleted: {
		if (note !== undefined) {
			text = NoteFile.richHtml(note.html)
		}
		lastLength = length
		loading = false
		cursorPosition = length
	}

	onTextChanged: {
		Qt.callLater(refreshShown) // formatting changes the text too
		if (loading || note === undefined) {
			return
		}
		var grown = length - lastLength
		lastLength = length
		applyPending(grown)
		Notes.typed(note, NoteFile.cleanHtml(text))
	}
	onSelectionStartChanged: Qt.callLater(refreshShown)
	onSelectionEndChanged: Qt.callLater(refreshShown)
	// A caret moved by hand drops a choice made for what is typed next.
	onCursorPositionChanged: {
		Qt.callLater(refreshShown)
		if (length === lastLength && Object.keys(pending).length > 0) {
			pending = ({})
		}
	}

	Keys.onPressed: function(event) {
		if (event.key === Qt.Key_Escape) {
			escaped()
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
			toggleFormat(attribute)
			event.accepted = true
		} else if (event.key === Qt.Key_V) {
			// pasted formatting of its own goes once it has landed
			Qt.callLater(function() {
				Notes.typed(note, NoteFile.cleanHtml(text))
				reloadText()
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
			root.forceActiveFocus()
			mouse.accepted = false
		}
	}

	// The span just typed, for the format chosen before it was.
	TextSelection {
		id: typed
		document: root.textDocument
	}

	// One character at a time, read for the strip; never written.
	TextSelection {
		id: probe
		document: root.textDocument
	}
}
