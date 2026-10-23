import Quickshell
import Quickshell.Io
import QtQuick

// Edits apply while the shell runs: a change to the config, a plugin's files
// or the shell's own reloads the whole shell. Quickshell watches only
// shell.qml, so every path under the roots gets a FileView of its own here.
Item {
	id: root

	// One edit is several inotify events and a file written in place is seen
	// mid-write: the reload waits for a settle that each event re-arms.
	readonly property int settleMs: 200

	Timer {
		id: settle
		interval: root.settleMs
		onTriggered: {
			console.log("[plugins] the config or a plugin changed: reloading")
			Quickshell.reload(false)
		}
	}

	function request() {
		settle.restart()
	}

	// On a file a FileView reports edits to it; on a directory, entries
	// appearing or going.
	Component {
		id: watcher
		FileView {
			watchChanges: true
			preload: false
			blockWrites: true
			printErrors: false
			onFileChanged: root.request()
		}
	}

	Process {
		id: lister
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: root.listed(text)
		}
		stderr: StdioCollector {}
	}

	// shell.json is left out of what is watched: ShellConfig has its own
	// watcher on it, which tells the host's writes from edits.
	property string configPath: ""

	// One process, once: every path worth watching under `dirs`. The
	// config's dir is left out: canvas.conf beside shell.json is rewritten on
	// every settings change and the shell never reads it, so a directory
	// watcher would reload the shell, and close the settings window, each time.
	function watch(dirs) {
		lister.command = ["find"].concat(dirs)
		lister.running = true
	}

	function listed(raw) {
		var lines = String(raw).split("\n")
		for (var i = 0; i < lines.length; i++) {
			if (lines[i] !== "" && lines[i] !== configPath) {
				watcher.createObject(root, {path: lines[i]})
			}
		}
	}
}
