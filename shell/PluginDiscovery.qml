import Quickshell.Io
import QtQuick
import "Manifest.js" as Manifest

// Finds the plugins on disk: the folders under the plugin roots, a plugin
// in a later root replacing one of the same id in an earlier one, each with
// a manifest that reads and fits this shell and that the config does not
// switch off. What cannot load is skipped with a line saying why.
Item {
	id: root

	// The plugin API version this shell offers; a manifest asking for more
	// is not loaded.
	required property int apiVersion
	// The config as it stands, for `plugins.<id>.enabled`.
	required property var config

	// The plugins that can load, in discovery order: [{id, dir, manifest}].
	signal found(var plugins)

	property var roots: []

	function discover(rootDirs) {
		roots = rootDirs
		lister.command = ["find"].concat(rootDirs,
			["-mindepth", "1", "-maxdepth", "1", "-type", "d", "-printf", "%H\t%p\n"])
		lister.running = true
	}

	// A root that does not exist is a line on stderr, which nothing wants.
	Process {
		id: lister
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: root.listed(text)
		}
		stderr: StdioCollector {}
	}

	// One blocking reader, pointed at each manifest in turn.
	FileView {
		id: reader
		blockLoading: true
		blockAllReads: true
		blockWrites: true
		printErrors: false
	}

	function readJson(path) {
		reader.path = path
		var raw = reader.text()
		if (raw === "") {
			return null
		}
		try {
			return JSON.parse(raw)
		} catch (e) {
			return null
		}
	}

	function listed(listing) {
		var candidates = Manifest.candidates(listing, roots)
		var plugins = []
		var ids = Object.keys(candidates)
		for (var i = 0; i < ids.length; i++) {
			var id = ids[i]
			var dir = candidates[id].dir
			var manifest = readJson(dir + "/plugin.json")
			var problem = Manifest.problem(id, manifest, apiVersion)
			if (problem === "" && config.plugins && config.plugins[id]
					&& config.plugins[id].enabled === false) {
				problem = "disabled in the config"
			}
			if (problem !== "") {
				Manifest.skip(dir, problem)
				continue
			}
			plugins.push({id: id, dir: dir, manifest: manifest})
		}
		found(plugins)
	}
}
