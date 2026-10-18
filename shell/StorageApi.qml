import Quickshell.Io
import QtQuick

// api.storage: the plugin's own directory under
// $XDG_STATE_HOME/wayland-canvas/plugins/<id>/, with read(name) and
// write(name, value) for JSON files by name, readText(name) and
// writeText(name, text) for any other file, list(callback) with the names
// of the files in it, and remove(name). read returns undefined when the
// file is not there or not valid JSON, readText an empty string when it is
// not there. A name is a file name in the directory, never a path.
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

	function plainName(name) {
		return typeof name === "string" && name !== "" && name !== "."
			&& name !== ".." && name.indexOf("/") < 0
	}

	function readText(name) {
		if (!plainName(name)) {
			return ""
		}
		file.path = dir + "/" + name
		return file.text()
	}

	function writeText(name, text) {
		if (!plainName(name)) {
			return
		}
		file.path = dir + "/" + name
		file.setText(text)
	}

	function read(name) {
		var raw = readText(name)
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
		writeText(name, JSON.stringify(value, null, "\t") + "\n")
	}

	Component {
		id: lister
		Process {
			stdout: StdioCollector {
				waitForEnd: true
			}
		}
	}

	Component {
		id: remover
		Process {}
	}

	// callback(names): the files in the directory, none when it is not
	// there yet.
	function list(callback) {
		var process = lister.createObject(root, {
			command: ["find", dir, "-mindepth", "1", "-maxdepth", "1", "-type", "f",
				"-printf", "%f\\n"]
		})
		process.stdout.streamFinished.connect(function() {
			var names = String(process.stdout.text).split("\n").filter(function(name) {
				return name !== ""
			})
			process.destroy()
			callback(names)
		})
		process.running = true
	}

	function remove(name) {
		if (!plainName(name)) {
			return
		}
		var process = remover.createObject(root, {
			command: ["rm", "-f", "--", dir + "/" + name]
		})
		process.exited.connect(function() {
			process.destroy()
		})
		process.running = true
	}
}
