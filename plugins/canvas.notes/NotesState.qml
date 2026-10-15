pragma Singleton

import QtQuick

// The notes the plugin owns, shared by the create action and every editor
// surface: the model (id -> {color, text, x, y, width, height}), the
// plugin's storage, and the compositor calls that keep the drawn cards in
// step. Every change — created, typed, recoloured, moved, deleted — is
// written to storage as it happens; at start every stored note is set
// again with its place, so a shell restart neither loses nor doubles one.
Item {
	id: root

	property var notes: ({})
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
		// Every stored note goes back to the compositor with its place:
		// setting a known id updates it, so nothing doubles.
		for (var id in notes) {
			var note = notes[id]
			compositor.call("item.set", {
				id: id,
				color: note.color,
				text: note.text,
				width: note.width,
				height: note.height,
				x: note.x,
				y: note.y,
			})
		}
	}

	function save() {
		if (storage !== null) {
			storage.write(storageName, {notes: notes, nextId: nextId})
		}
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
		compositor.call("item.set", {
			id: id,
			color: color,
			text: "",
			width: size,
			height: size,
		}, function(result, error) {
			if (result == null) {
				console.warn("[canvas.notes] could not place a new note: " + error)
				return // nowhere to place it: no note
			}
			notes[id] = {
				color: color,
				text: "",
				x: result.x,
				y: result.y,
				width: result.width,
				height: result.height,
			}
			save()
			compositor.call("item.activate", {id: id})
		})
	}

	// Typed text while an editor is open: kept at once. The drawn card
	// needs no update while the editor covers it.
	function typed(id, text) {
		var note = notes[id]
		if (note == null) {
			return
		}
		note.text = text
		save()
	}

	// The text of a closing editor: kept, or the note deleted when it never
	// got any.
	function setText(id, text) {
		var note = notes[id]
		if (note == null) {
			return
		}
		if (text.trim() === "") {
			remove(id)
			return
		}
		note.text = text
		save()
		compositor.call("item.set", {id: id, text: text})
	}

	// A colour dot in the editor.
	function recolor(id, color) {
		var note = notes[id]
		if (note == null) {
			return
		}
		note.color = color
		save()
		compositor.call("item.set", {id: id, color: color})
	}

	// The user dropped a dragged note.
	function moved(id, x, y) {
		var note = notes[id]
		if (note == null) {
			return
		}
		note.x = x
		note.y = y
		save()
	}

	function remove(id) {
		if (notes[id] == null) {
			return
		}
		delete notes[id]
		save()
		compositor.call("item.remove", {id: id})
	}

	Connections {
		target: root.compositor
		function onEvent(name, params) {
			if (name === "item.moved") {
				root.moved(params.id, params.x, params.y)
			}
		}
	}
}
