pragma Singleton

import QtQuick

// The notes the plugin owns: the model (id -> {color, text, x, y, width,
// height}) and the plugin's storage. Each note is drawn by a surface of
// the plugin's own, placed on the plane by the compositor
// (docs/intents/shell/notes.md), so nothing here draws or asks the
// compositor to draw. Every change — created, typed, recoloured, moved,
// deleted — is written to storage as it happens, and the stored notes
// come back as surfaces when the shell starts.
Item {
	id: root

	property var notes: ({})
	property var ids: []
	property int nextId: 1

	property var storage: null
	property var compositor: null
	property var colors: ({})
	property int size: 220
	// The colours for the editor's dots, in the manifest's own order: the
	// settings map arrives alphabetised (a QVariantMap sorts its keys), so
	// its order is not the manifest's — the manifest's order lives on in
	// its menu, the toolbar's colour menu — and a colour the menu leaves
	// out follows after, in the map's (alphabetical) order. Built once at
	// init; the dots bind to it directly.
	property var dotList: []

	readonly property string storageName: "notes.json"

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
		// Built whole, then assigned once: a var property notifies on
		// assignment, not on a push into the array it holds.
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
		var saved = storage.read(storageName)
		if (saved != null) {
			if (saved.notes != null) {
				notes = saved.notes
			}
			if (typeof saved.nextId === "number") {
				nextId = saved.nextId
			}
		}
		refresh()
	}

	function save() {
		if (storage !== null) {
			storage.write(storageName, {notes: notes, nextId: nextId})
		}
	}

	// The surfaces follow this list; it is re-made whenever a note is
	// added or removed, because a push into the array it holds would tell
	// no one.
	function refresh() {
		var next = []
		for (var id in notes) {
			next.push(id)
		}
		ids = next
		// A changed note is a changed map, and QML tells nobody about an
		// assignment of the same object: the map is copied, so everything
		// bound to a note sees the change.
		var copy = {}
		for (var key in notes) {
			copy[key] = notes[key]
		}
		notes = copy
	}

	// create {color}: a new note of that colour, centred on what the
	// output under the cursor shows (the overview's target when it is in
	// the overview — no place of its own is given), kept, then activated:
	// the activated event is what opens its editor, the same path a
	// double-click takes.
	function create(api, args) {
		init(api)
		var color = args != null && typeof args.color === "string" ? args.color : ""
		if (color === "") {
			for (var name in colors) {
				color = colors[name]
				break
			}
		}
		var id = "canvas.notes/" + nextId
		nextId++
		// Where the user is looking: the compositor reports each output's
		// canvas rectangle, and a new note goes in the middle of it.
		compositor.call("state", undefined, function(state, error) {
			var view = null
			if (state != null && state.outputs != null) {
				for (var i = 0; i < state.outputs.length; i++) {
					if (state.outputs[i].view != null) {
						view = state.outputs[i].view
						break
					}
				}
			}
			var x = view != null ? Math.round(view.x + view.width / 2 - size / 2) : 0
			var y = view != null ? Math.round(view.y + view.height / 2 - size / 2) : 0
			notes[id] = {
				color: color,
				text: "",
				x: x,
				y: y,
				width: size,
				height: size,
			}
			save()
			refresh()
		})
	}

	// What is typed into a note: kept as it is typed.
	function typed(id, text) {
		var note = notes[id]
		if (note == null || note.text === text) {
			return
		}
		note.text = text
		save()
	}

	// A colour dot on a note's strip.
	function recolour(id, color) {
		var note = notes[id]
		if (note == null) {
			return
		}
		note.color = color
		save()
		refresh()
	}

	// A note sized by hand, or grown to hold what was typed into it.
	function resized(id, width, height) {
		var note = notes[id]
		if (note == null) {
			return
		}
		var w = Math.round(width), h = Math.round(height)
		if (note.width === w && note.height === h) {
			return
		}
		note.width = w
		note.height = h
		save()
		refresh()
	}

	// A note dragged by its strip, or placed again after a drag.
	function moved(id, x, y) {
		var note = notes[id]
		if (note == null) {
			return
		}
		note.x = Math.round(x)
		note.y = Math.round(y)
		save()
		refresh()
	}

	function remove(id) {
		if (notes[id] == null) {
			return
		}
		// A `delete` on the map QML holds leaves the key behind: the map
		// is built again without it.
		var next = {}
		for (var other in notes) {
			if (other !== id) {
				next[other] = notes[other]
			}
		}
		notes = next
		save()
		refresh()
	}

}
