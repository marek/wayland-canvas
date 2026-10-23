import Quickshell
import Quickshell.Io
import QtQuick

// Edits apply while the shell runs: a change to the config, a plugin's
// files or the shell's own reloads the whole shell.
//
// Quickshell itself watches only the root shell.qml: a module the engine
// loaded from QML_IMPORT_PATH (the UI kit) and the plugins' files, compiled
// with Qt.createComponent from outside the shell dir, are not on its lists,
// so the host watches them itself. A FileView with watchChanges rides on
// inotify: pointed at a file it reports edits to it (a directory's watcher
// stays quiet on those), pointed at a directory it reports entries
// appearing or going (a plugin folder dropped in, a file added, an
// editor's rename-over). Every path under every root gets one.
Item {
	id: root

	// One change is seen as several inotify events, and a file written in
	// place is seen mid-write: reloading waits out a short settle that any
	// further event re-arms, so one burst of events — one human edit, one
	// finished write — becomes one reload. The settle runs only while a
	// change is landing: an idle shell has nothing running, no timer, no
	// process.
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

	// One process, once, when the shell comes up: every path worth
	// watching under `dirs`. The config's dir is not among them: the
	// dir's other entry is canvas.conf — the compositor's file, rewritten
	// through a temp file and a rename on every settings change, which
	// the shell never reads. A watcher on the dir cannot tell which entry
	// changed, so it would reload the shell, and close the settings
	// window, on each of them. The file's own watcher follows shell.json
	// through everything an editor or a hand does to it — saved over by
	// rename any number of times, deleted, created again — as the bar
	// scenario's config watcher steps prove.
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
