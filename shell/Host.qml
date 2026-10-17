import Quickshell
import Quickshell.Io
import QtQuick

// The plugin host (docs/intents/shell/plugins.md). It finds plugins in the
// built-in, user and configured directories — a plugin in a later directory
// replaces one with the same id in an earlier one — reads their manifests,
// skips what cannot load, with a line saying why, and hands every
// contributed component one api: plugin, storage, panels, actions, theme,
// compositor, shell. The panels are instantiated hidden and
// opened through the panel manager; the surfaces are instantiated here, one
// per output where a manifest asks, along with the registry of everything
// loaded for the surfaces that host others (the bar's widgets, the
// toolbar's buttons). Edits to the config or any plugin's files reload the
// whole shell with them. The host draws nothing itself: everything on
// screen belongs to a plugin.
Item {
	id: host

	// The plugin API this host offers; a manifest asking for more is not
	// loaded.
	readonly property int apiVersion: 1

	// The plugins that loaded, in discovery order: {id, dir, manifest, api}.
	property var plugins: []

	// Everything the plugins contributed, for the surfaces that host them:
	// barWidgets: {name, section, order, panel, component, api} — `panel`
	//   is the global name of the panel a left click opens, "" for none;
	//   a widget the config's bar.hidden names is left out of this list and
	//   still listed by api.shell.barWidgets, flagged;
	// toolbarButtons: {name, icon, label, action, args, panel, menu, order} —
	//   `action`, `panel` and every menu entry's action are global names.
	property var registry: ({barWidgets: [], toolbarButtons: []})

	// The screens, so per-output surfaces can follow them coming and going.
	property var screens: Quickshell.screens

	// ---- where things live ----

	// The built-in plugins sit beside the shell — <shell dir>/../plugins —
	// which is true both in the source tree and under
	// <datadir>/wayland-canvas once installed.
	readonly property string builtinDir: Quickshell.shellPath("../plugins")
	readonly property string userDir: xdgHome("XDG_DATA_HOME", ".local/share") + "/wayland-canvas/plugins"
	readonly property string configDir: xdgHome("XDG_CONFIG_HOME", ".config") + "/wayland-canvas"
	readonly property string configPath: configDir + "/shell.json"

	function xdgHome(variable, fallback) {
		var value = Quickshell.env(variable)
		return value !== "" && value != null ? value : Quickshell.env("HOME") + "/" + fallback
	}

	// Single-quotes `text` for /bin/sh.
	function shellQuote(text) {
		var out = "'"
		text = String(text)
		for (var i = 0; i < text.length; i++) {
			out += text[i] === "'" ? "'\\''" : text[i]
		}
		return out + "'"
	}

	// ---- the config: $XDG_CONFIG_HOME/wayland-canvas/shell.json ----

	property var config: ({})

	// The config file is watched for edits like everything else, with one
	// difference: the host writes it itself (setSetting, setMode), and one
	// such write is seen as several inotify events — a change inside this
	// window is taken to be that write, not an edit to reload on.
	readonly property int ownWriteGraceMs: 250
	property double lastConfigWrite: 0

	FileView {
		id: configFile
		path: host.configPath
		blockLoading: true
		blockAllReads: true
		blockWrites: true
		printErrors: false
		watchChanges: true
		onFileChanged: host.requestReload()
	}

	function loadConfig() {
		config = {}
		var raw = configFile.text()
		if (raw !== "") {
			try {
				config = JSON.parse(raw)
			} catch (e) {
				console.warn("[plugins] the config is not valid JSON (" + configPath + "): " + e)
			}
		}
	}

	// Persists the config; the shell keeps running on the values it already
	// applied, and the write is not mistaken for an edit. The parsed copy
	// reassigns `config` — a var property mutated in place fires no change
	// signal, and every binding on the config must run on the host's own
	// writes just as on a load.
	function writeConfig() {
		lastConfigWrite = Date.now()
		var raw = JSON.stringify(config, null, "\t") + "\n"
		configFile.setText(raw)
		config = JSON.parse(raw)
	}

	// ---- api.shell: the shell's own settings (the settings panel's) ----

	// Every bar widget in bar order, hidden ones included and flagged:
	// {name, pluginName, section, order, panel, component, api, hidden}.
	// Rebuilt with the registry, so a hide or show reaches the panel and
	// the bar together.
	property var barWidgets: []

	// The toolbar's edge, live from the config; "right" when unset.
	readonly property string toolbarPosition: {
		var position = config.toolbar ? config.toolbar.position : undefined
		return position === "left" || position === "top" || position === "bottom"
			|| position === "right" ? position : "right"
	}

	// Whether the config's bar.hidden names this widget.
	function barHidden(name) {
		var hidden = config.bar && config.bar.hidden ? config.bar.hidden : null
		return hidden != null && hidden.indexOf(name) >= 0
	}

	// Adds or removes one widget from bar.hidden. The bar loses or
	// regains the widget at once — the registry is re-derived, never the
	// whole shell reloaded.
	function setBarHidden(name, hidden) {
		var list = []
		var current = config.bar && config.bar.hidden ? config.bar.hidden : []
		for (var i = 0; i < current.length; i++) {
			if (current[i] !== name) {
				list.push(current[i])
			}
		}
		if (hidden) {
			list.push(name)
		}
		config.bar = config.bar || {}
		config.bar.hidden = list
		writeConfig()
		rebuildRegistry()
		return true
	}

	// Writes toolbar.position; the toolbar follows the live
	// toolbarPosition binding. Anything but an edge is refused.
	function setToolbarPosition(edge) {
		if (edge !== "right" && edge !== "left" && edge !== "top"
				&& edge !== "bottom") {
			return false
		}
		config.toolbar = config.toolbar || {}
		config.toolbar.position = edge
		writeConfig()
		rebuildRegistry()
		return true
	}

	// ---- the shared api pieces ----

	Compositor {
		id: compositor

		// A script's or keybinding's action.run arrives as an event: the
		// host runs the named plugin action, exactly as a toolbar button
		// would.
		onEvent: function(name, params) {
			if (name === "action") {
				actions.run(params.name, params.args)
			}
		}
	}

	// api.theme (and any plugin that wants the mode) talks to this one
	// connection.
	readonly property var compositorApi: compositor

	PanelsManager {
		id: panels
	}

	ActionsRunner {
		id: actions
	}

	ThemeApi {
		id: theme
		host: host
	}

	// ---- reading manifests and plugin state ----

	// One blocking reader, pointed at each file in turn.
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

	// ---- discovery: built-in, user, configured; later replaces earlier ----

	// Candidate plugins by id: {dir, root} with the discovery root's index.
	property var candidates: ({})

	Component.onCompleted: discover()

	function discover() {
		loadConfig()
		var roots = [builtinDir, userDir].concat(config.pluginPaths || [])
		var script = []
		for (var i = 0; i < roots.length; i++) {
			script.push("echo ROOT " + i + "; find " + shellQuote(roots[i])
				+ " -mindepth 1 -maxdepth 1 -type d 2>/dev/null")
		}
		lister.command = ["/bin/sh", "-c", script.join("; ")]
		lister.running = true
	}

	Process {
		id: lister
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: host.listed(text)
		}
	}

	function listed(raw) {
		var root = -1
		var lines = String(raw).split("\n")
		for (var i = 0; i < lines.length; i++) {
			var line = lines[i]
			if (line.indexOf("ROOT ") === 0) {
				root = Number(line.slice(5))
			} else if (line !== "" && root >= 0) {
				var id = line.slice(line.lastIndexOf("/") + 1)
				var earlier = candidates[id]
				if (earlier == null || root >= earlier.root) {
					candidates[id] = {dir: line, root: root}
				}
			}
		}
		loadPlugins()
	}

	// ---- loading: manifests, apis, contributions ----

	Component {
		id: pluginApiComponent
		PluginApi {}
	}

	Component {
		id: storageApiComponent
		StorageApi {}
	}

	property var surfaceEntries: [] // {component, api, perOutput, instances}

	function loadPlugins() {
		var loaded = []
		var storageDirs = []
		var ids = Object.keys(candidates)
		for (var i = 0; i < ids.length; i++) {
			var id = ids[i]
			var candidate = candidates[id]
			var manifest = readJson(candidate.dir + "/plugin.json")
			var problem = manifestProblem(id, manifest)
			if (problem !== "") {
				skip(candidate.dir, problem)
				continue
			}
			if (config.plugins && config.plugins[id]
					&& config.plugins[id].enabled === false) {
				skip(candidate.dir, "disabled in the config")
				continue
			}
			var pluginApi = pluginApiComponent.createObject(host,
				{host: host, pluginId: id, manifest: manifest})
			var storage = storageApiComponent.createObject(host,
				{dir: xdgHome("XDG_STATE_HOME", ".local/state") + "/wayland-canvas/plugins/" + id})
			storageDirs.push(storage.dir)
			loaded.push({
				id: id,
				dir: candidate.dir,
				manifest: manifest,
				api: {
					plugin: pluginApi,
					storage: storage,
					panels: panels,
					actions: actions,
					theme: theme,
					compositor: compositor,
					shell: host,
				},
			})
		}
		// Each plugin's storage directory appears when its api does.
		ensureDirs(storageDirs)
		plugins = loaded
		buildRegistry()
	}

	// The directories the shell owns, made with one process, once.
	function ensureDirs(dirs) {
		if (dirs.length === 0) {
			return
		}
		var script = "mkdir -p"
		for (var i = 0; i < dirs.length; i++) {
			script += " " + shellQuote(dirs[i])
		}
		mkdir.command = ["/bin/sh", "-c", script]
		mkdir.running = true
	}

	Process {
		id: mkdir
	}

	function manifestProblem(id, manifest) {
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

	// Compiles every named entry of every plugin, registers the panels and
	// actions, and fills the registry — staging each plugin's contributions
	// and taking them only when all of it worked, so one broken plugin is
	// skipped whole and the rest keep working.
	function buildRegistry() {
	var widgets = []
	var buttons = []
	var surfaces = []
	var kept = []
	for (var i = 0; i < plugins.length; i++) {
		var plugin = plugins[i]
		var contributes = plugin.manifest.contributes || {}
		var byKind = [
			["surfaces", contributes.surfaces],
			["barWidgets", contributes.barWidgets],
			["panels", contributes.panels],
			["actions", contributes.actions],
		]
		// Toolbar buttons are manifest data, not components: they name no
		// file, so there is nothing to compile — the toolbar that hosts
		// them skips a button that names none or more than one of action,
		// panel and menu.
		var toolbarEntries = contributes.toolbarButtons || []
		// Compile everything the manifest names first: one file that does
		// not compile skips the whole plugin, before anything registers.
		var compiled = []
		var problem = ""
		// A toolbar button is manifest data — icon, label, an action, a
		// panel or a menu — and names no file, so it compiles nothing.
		for (var j = 0; j < byKind.length && problem === ""; j++) {
			var entries = byKind[j][1] || []
			if (byKind[j][0] === "toolbarButtons") {
				for (var k = 0; k < entries.length; k++) {
					compiled.push(["toolbarButtons", entries[k], null])
				}
				continue
			}
			for (var k = 0; k < entries.length && problem === ""; k++) {
				var entry = entries[k]
				if (typeof entry.entry !== "string" || entry.entry === "") {
					problem = byKind[j][0] + " entry " + (entry.id || "?") + " names no file"
					break
				}
				var component = Qt.createComponent(plugin.dir + "/" + entry.entry)
				if (component.status === Component.Error) {
					problem = entry.entry + ": " + component.errorString()
					break
				}
				compiled.push([byKind[j][0], entry, component])
			}
		}
		if (problem !== "") {
			skip(plugin.dir, problem)
			continue
		}
		// Register what compiled: panels instantiate hidden, the rest are
		// collected, and a failure anywhere (a bad section, a panel that
		// will not instantiate) still skips the whole plugin.
		var pluginWidgets = []
		var pluginButtons = []
		var pluginSurfaces = []
		for (var c = 0; c < compiled.length && problem === ""; c++) {
			var kind = compiled[c][0]
			var e = compiled[c][1]
			var piece = compiled[c][2]
			if (kind === "surfaces") {
				pluginSurfaces.push({
					component: piece,
					api: plugin.api,
					perOutput: e.perOutput === true,
					instances: [],
				})
			} else if (kind === "barWidgets") {
				if (e.section !== "left" && e.section !== "center" && e.section !== "right") {
					problem = "bar widget " + e.id + " has no valid section"
					break
				}
				pluginWidgets.push({
					name: plugin.id + "/" + e.id,
					pluginName: plugin.manifest.name || plugin.id,
					section: e.section,
					order: e.order || 0,
					panel: e.panel ? plugin.id + "/" + e.panel : "",
					component: piece,
					api: plugin.api,
				})
			} else if (kind === "panels") {
				var instance = piece.createObject(host)
				if (instance == null) {
					problem = "panel " + e.id + " did not instantiate"
					break
				}
				instance.api = plugin.api
				panels.register(plugin.id + "/" + e.id, instance)
			} else if (kind === "actions") {
				actions.register(plugin.id + "/" + e.id, piece, plugin.api)
			}
		}
	// The toolbar buttons, straight from the manifest data.
	for (var t = 0; t < toolbarEntries.length; t++) {
		var b = toolbarEntries[t]
		pluginButtons.push({
			name: plugin.id + "/" + (b.id || "?"),
			icon: b.icon || "",
			label: b.label || "",
			action: b.action ? plugin.id + "/" + b.action : "",
			panel: b.panel ? plugin.id + "/" + b.panel : "",
			menu: (b.menu || []).map(function(entry) {
				return Object.assign({}, entry, {
					action: entry.action ? plugin.id + "/" + entry.action : ""
				})
			}),
			args: b.args,
			order: b.order || 0,
		})
	}
	if (problem !== "") {
		skip(plugin.dir, problem)
		continue
	}
	kept.push(plugin)
		widgets = widgets.concat(pluginWidgets)
		buttons = buttons.concat(pluginButtons)
		surfaces = surfaces.concat(pluginSurfaces)
	}
	plugins = kept
	surfaceEntries = surfaces
	allWidgets = widgets
	allButtons = buttons
	rebuildRegistry()
	start()
	}

	// Every contribution that loaded, before the config orders and filters
	// them: the registry is derived from these, so it can be re-derived when
	// the config changes without compiling or registering anything again.
	property var allWidgets: []
	property var allButtons: []

	// Rebuilds the registry (and the shell api's bar-widget list) from the
	// contributions and the config as it stands. A reload ends in
	// buildRegistry; the host's own config writes — which never reload —
	// end here, so what they change reaches every live binding.
	function rebuildRegistry() {
		var ordered = orderedWidgets(allWidgets)
		barWidgets = ordered
		registry = {
			barWidgets: ordered.filter(function(w) { return !w.hidden }),
			toolbarButtons: orderedButtons(allButtons),
		}
	}

	// One section's widgets: those the config's bar.layout lists, in the
	// listed order, then the rest in their declared order. Each entry is a
	// copy of the contribution carrying its hidden flag, so the bar can
	// leave hidden widgets out and the settings panel can list them marked.
	function widgetEntry(widget) {
		var entry = {}
		for (var key in widget) {
			entry[key] = widget[key]
		}
		entry.hidden = barHidden(widget.name)
		return entry
	}

	function orderedWidgets(all) {
		var out = []
		var sections = ["left", "center", "right"]
		for (var s = 0; s < sections.length; s++) {
			var section = all.filter(function(w) { return w.section === sections[s] })
			var layout = config.bar && config.bar.layout ? config.bar.layout[sections[s]] : null
			var rest = section.slice()
			if (layout != null) {
				for (var i = 0; i < layout.length; i++) {
					for (var j = 0; j < rest.length; j++) {
						if (rest[j].name === layout[i]) {
							out.push(widgetEntry(rest[j]))
							rest.splice(j, 1)
							break
						}
					}
				}
			}
			rest.sort(function(a, b) {
				return a.order - b.order || (a.name < b.name ? -1 : 1)
			})
			for (var r = 0; r < rest.length; r++) {
				out.push(widgetEntry(rest[r]))
			}
		}
		return out
	}

	// The toolbar buttons: those the config's toolbar.order lists, in the
	// listed order, then the rest in their declared order.
	function orderedButtons(all) {
		var out = []
		var rest = all.slice()
		var order = config.toolbar ? config.toolbar.order : null
		if (order != null) {
			for (var i = 0; i < order.length; i++) {
				for (var j = 0; j < rest.length; j++) {
					if (rest[j].name === order[i]) {
						out.push(rest[j])
						rest.splice(j, 1)
						break
					}
				}
			}
		}
		rest.sort(function(a, b) {
			return a.order - b.order || (a.name < b.name ? -1 : 1)
		})
		return out.concat(rest)
	}

	// ---- coming up: the theme, the surfaces, the watcher ----

	function start() {
		theme.start()
		startSurfaces()
		installWatchers()
	}

	function startSurfaces() {
		for (var i = 0; i < surfaceEntries.length; i++) {
			var entry = surfaceEntries[i]
			if (entry.perOutput) {
				for (var s = 0; s < screens.length; s++) {
					instantiateSurface(entry, screens[s])
				}
			} else {
				instantiateSurface(entry, null)
			}
		}
	}

	function instantiateSurface(entry, screen) {
		var instance = entry.component.createObject(host,
			{api: entry.api, registry: registry, modelData: screen})
		if (instance == null) {
			console.warn("[plugins] a surface of " + entry.api.plugin.manifest.id
				+ " did not instantiate")
			return
		}
		entry.instances.push(instance)
	}

	// Screens came or went: every per-output surface is rebuilt so each
	// screen keeps its own.
	onScreensChanged: {
		for (var i = 0; i < surfaceEntries.length; i++) {
			var entry = surfaceEntries[i]
			if (!entry.perOutput) {
				continue
			}
			for (var j = 0; j < entry.instances.length; j++) {
				entry.instances[j].destroy()
			}
			entry.instances = []
			for (var s = 0; s < screens.length; s++) {
				instantiateSurface(entry, screens[s])
			}
		}
	}

	// ---- edits apply while the shell runs ----

	// Quickshell itself watches only the root shell.qml: a module the
	// engine loaded from QML_IMPORT_PATH (the UI kit) and the plugins'
	// files, compiled with Qt.createComponent from outside the shell
	// dir, are not on its lists, so the host watches them itself. A
	// FileView with watchChanges rides on inotify: pointed at a file it
	// reports edits to it (a directory's watcher stays quiet on those),
	// pointed at a directory it reports entries appearing or going (a
	// plugin folder dropped in, a file added, an editor's rename-over).
	// Every path under every plugin root and under the shell's own dir
	// gets one.

	// One change is seen as several inotify events, and a file written
	// in place is seen mid-write: reloading waits out a short settle
	// that any further event re-arms, so one burst of events — one
	// human edit, one finished write — becomes one reload. The settle
	// runs only while a change is landing: an idle shell has nothing
	// running, no timer, no process.
	readonly property int settleMs: 200

	Timer {
		id: settle
		interval: host.settleMs
		onTriggered: {
			console.log("[plugins] the config or a plugin changed: reloading")
			Quickshell.reload(false)
		}
	}

	function requestReload() {
		if (Date.now() - lastConfigWrite < ownWriteGraceMs) {
			// The host's own config write, however the watcher saw it.
			return
		}
		settle.restart()
	}

	Component {
		id: fileWatcher
		FileView {
			watchChanges: true
			preload: false
			blockWrites: true
			printErrors: false
			onFileChanged: host.requestReload()
		}
	}

	// One process, once, when the shell comes up: every path worth
	// watching under the plugin roots and the shell's dir. The config's
	// dir is not among them: shell.json has its own watcher above, which
	// tells the host's writes from edits, and the dir's other entry is
	// canvas.conf — the compositor's file, rewritten through a temp file
	// and a rename on every settings change, which the shell never reads.
	// A watcher on the dir cannot tell which entry changed, so it would
	// reload the shell, and close the settings window, on each of them.
	// The file's own watcher follows shell.json through everything an
	// editor or a hand does to it — saved over by rename any number of
	// times, deleted, created again — as the bar scenario's config
	// watcher steps prove.
	Process {
		id: watchLister
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: host.watchListed(text)
		}
	}

	function installWatchers() {
		var dirs = [builtinDir, userDir, Quickshell.shellPath("")]
			.concat(config.pluginPaths || [])
		var script = []
		for (var i = 0; i < dirs.length; i++) {
			script.push("find " + shellQuote(dirs[i]) + " 2>/dev/null")
		}
		watchLister.command = ["/bin/sh", "-c", script.join("; ")]
		watchLister.running = true
	}

	function watchListed(raw) {
		var lines = String(raw).split("\n")
		for (var i = 0; i < lines.length; i++) {
			var path = lines[i]
			if (path === "" || path === configPath) {
				continue
			}
			fileWatcher.createObject(host, {path: path})
		}
	}
}
