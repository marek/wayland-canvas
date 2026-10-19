pragma Singleton

import QtQuick
import WaylandCanvas.Ui

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
	property var colors: ({})
	property int size: 220
	// The colours for the strip's dots, in the manifest's own order: the
	// settings map arrives alphabetised (a QVariantMap sorts its keys), so
	// its order is not the manifest's — the manifest's order lives on in
	// its menu, the toolbar's colour menu — and a colour the menu leaves
	// out follows after, in the map's (alphabetical) order.
	property var dotList: []

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
		colors = api.plugin.settings.colors
		size = api.plugin.settings.size
		var ordered = []
		var buttons = api.plugin.manifest != null
			&& api.plugin.manifest.contributes != null
			&& api.plugin.manifest.contributes.toolbarButtons != null
			? api.plugin.manifest.contributes.toolbarButtons : []
		for (var i = 0; i < buttons.length; i++) {
			var entries = buttons[i].menu || []
			for (var j = 0; j < entries.length; j++) {
				if (entries[j].color !== undefined && entries[j].color !== "") {
					ordered.push(entries[j].color)
				}
			}
		}
		for (var name in colors) {
			if (ordered.indexOf(colors[name]) === -1) {
				ordered.push(colors[name])
			}
		}
		var dots = []
		for (var k = 0; k < ordered.length; k++) {
			for (var key in colors) {
				if (colors[key] === ordered[k]) {
					dots.push({name: key, color: ordered[k]})
					break
				}
			}
		}
		dotList = dots
		storage.list(load)
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
			storage.writeText(note.id + fileSuffix, noteFile(note))
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
			for (var name in colors) {
				color = colors[name]
				break
			}
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

	// ---- the HTML of a note ----

	// Qt's rich text, as a TextArea holds it, down to plain HTML: the
	// paragraphs, line breaks, the b, i and u spans and a span's size, and
	// nothing of fonts or colours — the font is the note's, not a span's.
	function cleanHtml(rich) {
		var body = rich
		var start = rich.indexOf("<body")
		if (start >= 0) {
			var end = rich.lastIndexOf("</body>")
			body = rich.substring(rich.indexOf(">", start) + 1, end >= 0 ? end : rich.length)
		}
		body = body.replace(/<span style="([^"]*)">([\s\S]*?)<\/span>/g,
			function(all, style, inner) {
				var open = "", close = ""
				// Written as px; pasted text may come in pt.
				var size = /font-size:\s*(\d+(?:\.\d+)?)(px|pt)/.exec(style)
				if (size !== null) {
					var px = Math.round(Number(size[1]) * (size[2] === "pt" ? 4 / 3 : 1))
					open += "<x-size " + px + ">"
					close = "</x-size>"
				}
				var weight = /font-weight:\s*(\d+)/.exec(style)
				if (weight !== null && Number(weight[1]) >= 600) {
					open += "<b>"
					close = "</b>" + close
				}
				if (/font-style:\s*italic/.test(style)) {
					open += "<i>"
					close = "</i>" + close
				}
				if (/text-decoration:[^;"]*underline/.test(style)) {
					open += "<u>"
					close = "</u>" + close
				}
				return open + inner + close
			})
		// Every tag but these goes, its text kept; the size marks above
		// become the plain span they stand for.
		body = body.replace(/<(\/?)([a-zA-Z][a-zA-Z0-9-]*)([^>]*)>/g,
			function(all, slash, tag, rest) {
				tag = tag.toLowerCase()
				if (tag === "br") {
					return "<br>"
				}
				if (tag === "p" || tag === "b" || tag === "i" || tag === "u") {
					return "<" + slash + tag + ">"
				}
				if (tag === "x-size") {
					return slash !== "" ? "</span>"
						: "<span style=\"font-size: " + rest.trim() + "px\">"
				}
				return ""
			})
		return body.replace(/^\s+|\s+$/g, "")
	}

	// Plain HTML back into a TextArea: paragraphs without Qt's own margins,
	// spaces kept, an empty paragraph as one empty line.
	function richHtml(html) {
		var marked = html.replace(/<p><br><\/p>/g,
			"<p style=\"-qt-paragraph-type:empty\"><br /></p>")
		return "<html><head><style>p { margin-top: 0px; margin-bottom: 0px; "
			+ "white-space: pre-wrap; }</style></head><body>" + marked + "</body></html>"
	}

	function cssString(text) {
		return "\"" + String(text).replace(/\\/g, "\\\\").replace(/"/g, "\\\"") + "\""
	}

	// The whole file: the text, the colour, the font and the size of the
	// text that names none as the page's style, so a browser shows the
	// note as the card does, and the note's canvas rectangle in a meta
	// element.
	function noteFile(note) {
		return "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
			+ "<meta name=\"canvas-note\" content=\"x=" + note.x + " y=" + note.y
			+ " width=" + note.width + " height=" + note.height + "\">\n"
			+ "<style>\nbody { background: " + note.color + "; color: " + ink
			+ "; font-family: " + cssString(note.font) + "; font-size: " + note.size
			+ "px; }\np { margin-top: 0; margin-bottom: 0; white-space: pre-wrap; }\n"
			+ "</style>\n</head>\n<body>\n" + note.html + "\n</body>\n</html>\n"
	}

	// A file back into a note; what it leaves out takes the defaults, so a
	// page written by hand is a note too.
	function parseNote(id, text) {
		var place = {}
		var meta = /<meta\s+name="canvas-note"\s+content="([^"]*)"/i.exec(text)
		if (meta !== null) {
			meta[1].replace(/(\w+)=(-?\d+)/g, function(all, key, value) {
				place[key] = Number(value)
			})
		}
		var style = /<style[^>]*>([\s\S]*?)<\/style>/i.exec(text)
		var rule = style !== null ? /body\s*\{([^}]*)\}/.exec(style[1]) : null
		var css = rule !== null ? rule[1] : ""
		var background = /background(?:-color)?:\s*(#[0-9a-fA-F]{3,8})/.exec(css)
		var family = /font-family:\s*"((?:[^"\\]|\\.)*)"/.exec(css)
		var px = /font-size:\s*(\d+)px/.exec(css)
		var body = /<body[^>]*>([\s\S]*?)<\/body>/i.exec(text)
		var first = dotList.length > 0 ? dotList[0].color : "#ffd866"
		return noteComponent.createObject(root, {
			id: id,
			color: background !== null ? background[1] : first,
			html: body !== null ? body[1].replace(/^\s+|\s+$/g, "") : "",
			x: place.x !== undefined ? place.x : 0,
			y: place.y !== undefined ? place.y : 0,
			width: place.width > 0 ? place.width : size,
			height: place.height > 0 ? place.height : size,
			font: family !== null ? family[1].replace(/\\(.)/g, "$1") : UiTheme.fontFamily,
			size: px !== null ? Number(px[1]) : UiTheme.fontBody,
		})
	}
}
