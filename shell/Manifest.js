.pragma library

// What a plugin's manifest and folder say, read one way by every part of the
// host (docs/intents/shell/plugins.md).

// The name a plugin's panel, action, widget or button goes by in every
// manifest and in the API: "<plugin id>/<local id>". No local id, no name.
function globalName(pluginId, localId) {
	return localId ? pluginId + "/" + localId : ""
}

// Why the manifest read from folder `id` cannot load under API version
// `apiVersion`; "" when it can.
function problem(id, manifest, apiVersion) {
	if (manifest == null) {
		return "no valid plugin.json"
	}
	if (manifest.id !== id) {
		return "the manifest id does not match the folder name"
	}
	if (typeof manifest.apiVersion !== "number" || !Number.isInteger(manifest.apiVersion)) {
		return "apiVersion is not an integer"
	}
	if (manifest.apiVersion > apiVersion) {
		return "needs API version " + manifest.apiVersion + ", the shell offers " + apiVersion
	}
	if (manifest.contributes != null && typeof manifest.contributes !== "object") {
		return "contributes is not an object"
	}
	return ""
}

function skip(dir, reason) {
	console.warn("[plugins] skipping " + dir + ": " + reason)
}

// The plugin folders a listing names, one per id: {id: {dir, root}}. A line
// is the root a folder was found under, a tab and the folder's path; a
// folder in a later root replaces one of the same name in an earlier root.
function candidates(listing, roots) {
	var found = {}
	var lines = String(listing).split("\n")
	for (var i = 0; i < lines.length; i++) {
		var tab = lines[i].indexOf("\t")
		if (tab < 0) {
			continue
		}
		var dir = lines[i].slice(tab + 1)
		var root = roots.lastIndexOf(lines[i].slice(0, tab))
		var id = dir.slice(dir.lastIndexOf("/") + 1)
		if (root >= 0 && (found[id] === undefined || root >= found[id].root)) {
			found[id] = {dir: dir, root: root}
		}
	}
	return found
}
