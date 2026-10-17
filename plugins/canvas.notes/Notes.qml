import Quickshell
import QtQuick
import "."

// Every note the plugin owns, one surface each on the plane. The surface
// is the note: the same one whether it is being written in or not, so
// there is never a second card to drift away from the first
// (docs/intents/shell/notes.md). The compositor only places them.
Item {
	id: root

	property var api: null
	property var registry: null
	property var modelData: null

	Component.onCompleted: Notes.init(api)

	Repeater {
		model: Notes.ids

		delegate: NoteSurface {
			required property string modelData

			noteId: modelData
			api: root.api
		}
	}
}
