import Quickshell.Io
import QtQuick

// api.storage: the plugin's own directory under
// $XDG_STATE_HOME/wayland-canvas/plugins/<id>/, with read(name) and
// write(name, value) for JSON files by name. read returns undefined when
// the file is not there or not valid JSON.
Item {
	id: root

	property string dir: ""

	FileView {
		id: file
		blockLoading: true
		blockAllReads: true
		blockWrites: true
		printErrors: false
	}

	function read(name) {
		file.path = dir + "/" + name
		var raw = file.text()
		if (raw === "") {
			return undefined
		}
		try {
			return JSON.parse(raw)
		} catch (e) {
			return undefined
		}
	}

	function write(name, value) {
		file.path = dir + "/" + name
		file.setText(JSON.stringify(value, null, "\t") + "\n")
	}
}
