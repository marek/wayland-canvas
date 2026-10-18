import QtQuick
import "."

// canvas.notes/create {color}: a note of that colour at the centre of what
// the overview is aimed at; the plugin's shared state keeps it, and its
// surface takes the keyboard as it maps.
Item {
	property var api: null

	function run(args) {
		Notes.create(api, args)
	}
}
