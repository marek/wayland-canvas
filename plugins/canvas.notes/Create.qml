import QtQuick
import "."

// canvas.notes/create {color}: a note of that colour at the centre of what
// the overview is aimed at, then its editor — the plugin's shared state
// does the placing and the activating.
Item {
	property var api: null

	function run(args) {
		Notes.create(api, args)
	}
}
