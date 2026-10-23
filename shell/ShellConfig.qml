import Quickshell
import Quickshell.Io
import QtQuick

// Where the shell's files live, and shell.json: read once at start, written
// only by set(), watched for edits made from outside. The shell keeps
// running on the values it already applied; an edit from outside ends in a
// reload (ReloadWatcher), never in a patch of the live state.
Item {
	id: root

	// The built-in plugins sit beside the shell — <shell dir>/../plugins —
	// which is true both in the source tree and under
	// <datadir>/wayland-canvas once installed.
	readonly property string builtinDir: Quickshell.shellPath("../plugins")
	readonly property string userDir: xdgHome("XDG_DATA_HOME", ".local/share") + "/wayland-canvas/plugins"
	readonly property string stateDir: xdgHome("XDG_STATE_HOME", ".local/state") + "/wayland-canvas/plugins"
	readonly property string configDir: xdgHome("XDG_CONFIG_HOME", ".config") + "/wayland-canvas"
	readonly property string path: configDir + "/shell.json"

	// The config as it stands: reassigned on a load and on every set(), so
	// a binding on it runs on the host's own writes just as on a load.
	property var data: ({})

	// False while shell.json holds text that is not a JSON object (mid-edit
	// or mistyped): a write would replace the user's file. A missing or
	// blank file has nothing to lose.
	property bool valid: true
	property bool skipLogged: false

	// shell.json changed by anything but set().
	signal edited()

	// What set() last wrote: a change that leaves the file holding it is
	// set()'s own, however late it is seen. Null until the first write.
	property var lastWritten: null

	function xdgHome(variable, fallback) {
		var value = Quickshell.env(variable)
		return value !== "" && value != null ? value : Quickshell.env("HOME") + "/" + fallback
	}

	// The directories plugins are found in, later replacing earlier: the
	// built-in, the user's and the config's pluginPaths.
	function pluginRoots() {
		return [builtinDir, userDir].concat(data.pluginPaths || [])
	}

	FileView {
		id: file
		path: root.path
		blockLoading: true
		blockAllReads: true
		blockWrites: true
		printErrors: false
		watchChanges: true
		onFileChanged: {
			file.reload()
			if (root.lastWritten === null || file.text() !== root.lastWritten) {
				root.edited()
			}
		}
	}

	function load() {
		data = {}
		valid = true
		skipLogged = false
		var raw = file.text()
		if (raw.trim() === "") {
			return
		}
		try {
			var parsed = JSON.parse(raw)
			if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
				throw new Error("not an object")
			}
			data = parsed
		} catch (e) {
			valid = false
			console.warn("[plugins] the config is not valid JSON (" + path + "): " + e)
		}
	}

	// Sets the value at the `keys` path (a list: a plugin id has dots in it),
	// making the objects on the way, in memory and in the file. A file that
	// did not parse is left as written: the change lives in this run until an
	// edit that parses reloads the shell.
	function set(keys, value) {
		var node = data
		for (var i = 0; i < keys.length - 1; i++) {
			if (node[keys[i]] === null || typeof node[keys[i]] !== "object") {
				node[keys[i]] = {}
			}
			node = node[keys[i]]
		}
		node[keys[keys.length - 1]] = value
		var raw = JSON.stringify(data, null, "\t") + "\n"
		if (valid) {
			lastWritten = raw
			file.setText(raw)
		} else if (!skipLogged) {
			skipLogged = true
			console.warn("[plugins] the config is not valid JSON: changes apply in memory "
				+ "only, and " + path + " is not written over")
		}
		data = JSON.parse(raw)
	}
}
