pragma Singleton

import QtQuick
import WaylandCanvas.Ui
import "noteFile.js" as NoteFile

// The notes the plugin owns (docs/intents/shell/notes.md): one Note object
// per note, changed in place — its properties tell everything bound to
// them, the note's surface and its placement among them, what changed — and
// the plugin's storage, where each note is a plain-HTML file of its own,
// `<id>.html`, written at every change and read back when the shell starts.
Item {
	id: root

	component Note: QtObject {
		property string id
		property string color
		// The text as plain HTML: paragraphs, line breaks, b, i and u, and
		// the size of a span that has one of its own.
		property string html
		property int x
		property int y
		property int width
		property int height
		property string font
		property int size // of the text that names no size of its own
	}

	Component {
		id: noteComponent
		Note {}
	}

	property var notes: ({}) // id -> Note
	property var ids: [] // the surfaces follow this list
	property int nextId: 1
	// The files are read once, asynchronously: a note created before they
	// are in waits for them, so it never takes the id of one on disk.
	property bool loaded: false
	property var waiting: []

	property var storage: null
	property var compositor: null
	property int size: 220
	// The colours the strip's dots offer, in the order of the plugin's
	// `colors` setting (the toolbar's colour menu is built from the same
	// list): [{label, color}].
	property var dotList: []
	readonly property string defaultColor: dotList.length > 0 ? dotList[0].color : ""

	readonly property string ink: "#1f2430"
	readonly property string fileSuffix: ".html"

	// The first plugin component to come up brings the api; there is one
	// model per shell, rebuilt by a reload.
	function init(api) {
		if (storage !== null) {
			return
		}
		storage = api.storage
		compositor = api.compositor
		size = api.plugin.settings.size
		dotList = colorList(api.plugin.settings.colors)
		if (dotList.length === 0) {
			dotList = colorList(api.plugin.manifest.settings.colors)
		}
		storage.list(load)
	}

	// The entries of a `colors` setting that name a colour; a hand-edited
	// config can hold anything there.
	function colorList(list) {
		return list == null || typeof list.filter !== "function" ? [] : list.filter(function(entry) {
			return entry != null && typeof entry.color === "string" && entry.color !== ""
		})
	}

	function load(names) {
		var found = {}
		var order = []
		var highest = 0
		for (var i = 0; i < names.length; i++) {
			var match = /^(\d+)\.html$/.exec(names[i])
			if (match === null) {
				continue
			}
			var note = parseNote(match[1], storage.readText(names[i]))
			found[note.id] = note
			order.push(note.id)
			highest = Math.max(highest, Number(match[1]))
		}
		order.sort(function(a, b) { return Number(a) - Number(b) })
		notes = found
		ids = order
		nextId = highest + 1
		loaded = true
		var asked = waiting
		waiting = []
		for (var j = 0; j < asked.length; j++) {
			create(null, asked[j])
		}
	}

	function save(note) {
		if (storage !== null) {
			storage.writeText(note.id + fileSuffix, NoteFile.encode(note, ink))
		}
	}

	// create {color}: a new note of that colour, centred on the cursor and
	// kept clear of the chrome of the screen it lands on; its surface takes
	// the keyboard as it maps.
	function create(api, args) {
		if (api !== null) {
			init(api)
		}
		if (!loaded) {
			waiting = waiting.concat([args])
			return
		}
		var color = args != null && typeof args.color === "string" ? args.color : ""
		if (color === "") {
			color = defaultColor
		}
		var id = String(nextId)
		nextId++
		compositor.call("state", undefined, function(state, error) {
			var spot = noteSpot(state)
			var note = noteComponent.createObject(root, {
				id: id,
				color: color,
				html: "",
				x: spot.x,
				y: spot.y,
				width: size,
				height: size,
				font: UiTheme.fontFamily,
				size: UiTheme.fontBody,
			})
			var next = Object.assign({}, notes)
			next[id] = note
			notes = next
			ids = ids.concat([id])
			save(note)
		})
	}

	// The canvas rectangle the user is looking at, when there is no cursor
	// to centre on: a screen in the overview first — the toolbar's — else
	// the first screen's.
	function shownView(state) {
		var outputs = state != null && state.outputs != null ? state.outputs : []
		for (var i = 0; i < outputs.length; i++) {
			if (outputs[i].overview === true && outputs[i].view != null) {
				return outputs[i].view
			}
		}
		for (var j = 0; j < outputs.length; j++) {
			if (outputs[j].view != null) {
				return outputs[j].view
			}
		}
		return null
	}

	// A new note's place: centred on the cursor's canvas point, moved only
	// as far as it takes to lie in the free area of the screen the cursor
	// is on — what the bars and the overview's strips leave of that
	// screen's view. With no cursor to centre on, the centre of what a
	// screen in the overview shows (the first screen's view when none is).
	function noteSpot(state) {
		var cursor = state != null && state.cursor != null ? state.cursor : null
		var outputs = state != null && state.outputs != null ? state.outputs : []
		var on = null
		if (cursor != null) {
			for (var i = 0; i < outputs.length; i++) {
				if (outputs[i].name === cursor.output) {
					on = outputs[i]
				}
			}
		}
		if (on == null) {
			var view = shownView(state)
			return {
				x: view !== null ? Math.round(view.x + view.width / 2 - size / 2) : 0,
				y: view !== null ? Math.round(view.y + view.height / 2 - size / 2) : 0,
			}
		}
		var free = on.usable != null ? on.usable : on.view
		return {
			x: Math.round(clamp(cursor.x - size / 2, free.x, free.x + free.width - size)),
			y: Math.round(clamp(cursor.y - size / 2, free.y, free.y + free.height - size)),
		}
	}

	// `value` into [low, high], as far as it takes and no further.
	function clamp(value, low, high) {
		return Math.max(low, Math.min(high, value))
	}

	function typed(note, html) {
		if (note.html === html) {
			return
		}
		note.html = html
		save(note)
	}

	function recolour(note, color) {
		note.color = color
		save(note)
	}

	function moved(note, x, y) {
		note.x = Math.round(x)
		note.y = Math.round(y)
		save(note)
	}

	function resized(note, width, height) {
		var w = Math.round(width), h = Math.round(height)
		if (note.width === w && note.height === h) {
			return
		}
		note.width = w
		note.height = h
		save(note)
	}

	function setFont(note, family) {
		note.font = family
		save(note)
	}

	function remove(note) {
		if (notes[note.id] !== note) {
			return
		}
		// The surface goes with its id; the object once nothing reads it.
		ids = ids.filter(function(id) { return id !== note.id })
		var next = {}
		for (var id in notes) {
			if (id !== note.id) {
				next[id] = notes[id]
			}
		}
		notes = next
		if (storage !== null) {
			storage.remove(note.id + fileSuffix)
		}
		Qt.callLater(function() { note.destroy() })
	}

	// A file's note; what the file leaves out takes the plugin's defaults.
	function parseNote(id, text) {
		var note = NoteFile.parse(text, {
			color: defaultColor,
			x: 0,
			y: 0,
			width: size,
			height: size,
			font: UiTheme.fontFamily,
			size: UiTheme.fontBody,
		})
		note.id = id
		return noteComponent.createObject(root, note)
	}
}
