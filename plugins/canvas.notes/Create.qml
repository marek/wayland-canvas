import QtQuick
import "."

// canvas.notes/create {color}: a note of that colour centred on the cursor,
// kept on its screen and clear of the toolbar and the bars; the plugin's
// shared state keeps it, and its surface takes the keyboard as it maps.
Item {
	property var api: null

	function run(args) {
		Notes.create(api, args)
	}
}
