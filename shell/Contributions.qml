import QtQuick
import "Manifest.js" as Manifest

// What the loaded plugins contribute (docs/intents/shell/plugins.md): the
// bar widgets, the toolbar buttons, the surfaces, the panels and the
// actions their manifests name. A plugin is taken all or nothing: every
// piece is compiled and staged first, and only a plugin whose pieces all
// worked is registered, so one broken plugin is skipped whole and the rest
// keep working.
Item {
	id: root

	// Where panels and actions register.
	required property var panelsManager
	required property var actionsRunner
	// The config as ShellConfig holds it, for the order and the hidden list.
	required property var config

	// The plugins that loaded: [{id, dir, manifest, api}].
	property var plugins: []

	// Every bar widget in bar order, hidden ones included and flagged:
	// {name, pluginName, section, order, panel, description, component,
	// api, hidden}. `panel` is the global name of the panel a left click
	// opens, "" for none, and `description` the manifest's optional line
	// about the widget, "" for none.
	property var barWidgets: []

	// The valid toolbar buttons in toolbar order: {name, icon, label,
	// action, args, panel, menu, order}; `action`, `panel` and every menu
	// entry's action are global names.
	property var toolbarButtons: []

	// The surfaces to make: {component, api, perOutput}.
	property var surfaces: []

	// Every widget and button that loaded, before the config orders and
	// filters them: barWidgets and toolbarButtons are derived from these,
	// so they can be derived again when the config changes without
	// compiling or registering anything again.
	property var allWidgets: []
	property var allButtons: []

	// One row per kind of contribution a manifest lists:
	//   key       the manifest's list;
	//   file      its entries name a QML file, compiled before anything is
	//             staged;
	//   validate  what is wrong with an entry as the manifest states it,
	//             "" for nothing;
	//   stage     makes the entry's piece into `staged`, taken only with
	//             the rest of its plugin; "" or what went wrong.
	readonly property var kinds: [
		{key: "surfaces", file: true, validate: nothingWrong, stage: stageSurface},
		{key: "barWidgets", file: true, validate: widgetProblem, stage: stageWidget},
		{key: "panels", file: true, validate: nothingWrong, stage: stagePanel},
		{key: "actions", file: true, validate: nothingWrong, stage: stageAction},
		{key: "toolbarButtons", file: false, validate: nothingWrong, stage: stageButton},
	]

	function nothingWrong(entry) {
		return ""
	}

	function ownName(plugin, entry) {
		return Manifest.globalName(plugin.id, entry.id || "?")
	}

	function widgetProblem(entry) {
		return entry.section !== "left" && entry.section !== "center" && entry.section !== "right"
			? "bar widget " + entry.id + " has no valid section" : ""
	}

	function stageSurface(plugin, entry, component, staged) {
		staged.surfaces.push({
			component: component,
			api: plugin.api,
			perOutput: entry.perOutput === true,
		})
		return ""
	}

	function stageWidget(plugin, entry, component, staged) {
		staged.widgets.push({
			name: ownName(plugin, entry),
			pluginName: plugin.manifest.name || plugin.id,
			section: entry.section,
			order: entry.order || 0,
			panel: Manifest.globalName(plugin.id, entry.panel),
			description: entry.description || "",
			component: component,
			api: plugin.api,
		})
		return ""
	}

	// A panel is made hidden: the panel manager opens it by its name.
	function stagePanel(plugin, entry, component, staged) {
		var instance = component.createObject(root, {api: plugin.api, panelName: entry.id})
		if (instance == null) {
			return "panel " + entry.id + " did not instantiate"
		}
		staged.panels.push({name: ownName(plugin, entry), instance: instance})
		return ""
	}

	function stageAction(plugin, entry, component, staged) {
		staged.actions.push({name: ownName(plugin, entry), component: component, api: plugin.api})
		return ""
	}

	// A toolbar button is manifest data — icon, label, an action, a panel
	// or a menu — and names no file. One naming none or more than one of
	// action, panel and menu is dropped, with a line saying why, so the
	// toolbar only ever sees valid ones; the plugin loads without it.
	function stageButton(plugin, entry, component, staged) {
		var button = {
			name: ownName(plugin, entry),
			icon: entry.icon || "",
			label: entry.label || "",
			action: Manifest.globalName(plugin.id, entry.action),
			panel: Manifest.globalName(plugin.id, entry.panel),
			menu: buttonMenu(plugin, entry.menu),
			args: entry.args,
			order: entry.order || 0,
		}
		var named = (button.action !== "" ? 1 : 0) + (button.panel !== "" ? 1 : 0)
			+ (button.menu.length > 0 ? 1 : 0)
		if (named === 1) {
			staged.buttons.push(button)
			return ""
		}
		console.warn("[plugins] skipping toolbar button " + button.name
			+ ": it names " + (named === 0
				? "no action, panel or menu"
				: "more than one of action, panel and menu"))
		return ""
	}

	// A button's submenu: the entries it declares, or the entries of the
	// plugin setting its `menu` names ({setting, action}), each of which
	// runs that action with the entry itself as its args. Action names
	// become global. A list read from a manifest is not a real array
	// (Array.isArray says no), so lists are told by their `map`.
	function buttonMenu(plugin, menu) {
		var entries = menu || []
		if (typeof entries.map !== "function") {
			var list = plugin.api.plugin.settings[menu.setting]
			entries = list != null && typeof list.map === "function"
				? list.map(function(item) {
					return Object.assign({}, item, {action: menu.action, args: item})
				}) : []
		}
		return entries.map(function(entry) {
			return Object.assign({}, entry, {action: Manifest.globalName(plugin.id, entry.action)})
		})
	}

	// The entry's component, compiled from its plugin's folder; what went
	// wrong, "" for nothing.
	function compile(plugin, item) {
		if (typeof item.entry.entry !== "string" || item.entry.entry === "") {
			return item.kind.key + " entry " + (item.entry.id || "?") + " names no file"
		}
		item.component = Qt.createComponent(plugin.dir + "/" + item.entry.entry)
		return item.component.status === Component.Error
			? item.entry.entry + ": " + item.component.errorString() : ""
	}

	// Everything the plugin's manifest names, made but not taken. Every file
	// compiles before any piece is made: one that does not skips the plugin
	// before anything exists.
	function stagePlugin(plugin) {
		var staged = {
			problem: "",
			surfaces: [],
			widgets: [],
			buttons: [],
			panels: [],
			actions: [],
		}
		var contributes = plugin.manifest.contributes || {}
		var work = []
		for (var k = 0; k < kinds.length; k++) {
			var entries = contributes[kinds[k].key] || []
			for (var e = 0; e < entries.length; e++) {
				var item = {kind: kinds[k], entry: entries[e], component: null}
				staged.problem = kinds[k].file ? compile(plugin, item) : ""
				if (staged.problem === "") {
					staged.problem = kinds[k].validate(item.entry)
				}
				if (staged.problem !== "") {
					return staged
				}
				work.push(item)
			}
		}
		for (var w = 0; w < work.length; w++) {
			staged.problem = work[w].kind.stage(plugin, work[w].entry, work[w].component, staged)
			if (staged.problem !== "") {
				return staged
			}
		}
		return staged
	}

	// A skipped plugin leaves nothing behind: the panels made for it go.
	function discard(staged) {
		for (var i = 0; i < staged.panels.length; i++) {
			staged.panels[i].instance.destroy()
		}
	}

	function register(staged) {
		for (var i = 0; i < staged.panels.length; i++) {
			panelsManager.register(staged.panels[i].name, staged.panels[i].instance)
		}
		for (var j = 0; j < staged.actions.length; j++) {
			actionsRunner.register(staged.actions[j].name, staged.actions[j].component,
				staged.actions[j].api)
		}
	}

	// Takes each of `loaded` — [{id, dir, manifest, api}] — whole or not at
	// all.
	function load(loaded) {
		var kept = []
		var widgets = []
		var buttons = []
		var made = []
		for (var i = 0; i < loaded.length; i++) {
			var staged = stagePlugin(loaded[i])
			if (staged.problem !== "") {
				discard(staged)
				Manifest.skip(loaded[i].dir, staged.problem)
				continue
			}
			register(staged)
			kept.push(loaded[i])
			widgets = widgets.concat(staged.widgets)
			buttons = buttons.concat(staged.buttons)
			made = made.concat(staged.surfaces)
		}
		plugins = kept
		surfaces = made
		allWidgets = widgets
		allButtons = buttons
		reorder()
	}

	// Orders the bar widgets and the toolbar buttons from the contributions
	// and the config as it stands. Only bar.layout, bar.hidden and
	// toolbar.order feed it: a load ends here, and the host's one write of
	// those, bar.hidden, calls it. Every other write leaves the live
	// widgets alone, so what they hold survives it.
	function reorder() {
		barWidgets = orderedWidgets()
		toolbarButtons = orderByConfig(allButtons,
			config.data.toolbar ? config.data.toolbar.order : null)
	}

	// Whether the config's bar.hidden names this widget.
	function hidden(name) {
		var list = config.data.bar && config.data.bar.hidden ? config.data.bar.hidden : null
		return list != null && list.indexOf(name) >= 0
	}

	// The items the config lists, in the listed order, then the rest in
	// their declared order.
	function orderByConfig(items, listed) {
		var out = []
		var rest = items.slice()
		for (var i = 0; listed != null && i < listed.length; i++) {
			for (var j = 0; j < rest.length; j++) {
				if (rest[j].name === listed[i]) {
					out.push(rest[j])
					rest.splice(j, 1)
					break
				}
			}
		}
		rest.sort(function(a, b) {
			return a.order - b.order || (a.name < b.name ? -1 : 1)
		})
		return out.concat(rest)
	}

	// Each section's widgets in turn, each a copy of the contribution
	// carrying its hidden flag: the bar leaves hidden widgets out and the
	// settings panel lists them marked.
	function orderedWidgets() {
		var out = []
		var sections = ["left", "center", "right"]
		var layout = config.data.bar && config.data.bar.layout ? config.data.bar.layout : {}
		for (var s = 0; s < sections.length; s++) {
			var inSection = allWidgets.filter(function(w) { return w.section === sections[s] })
			var ordered = orderByConfig(inSection, layout[sections[s]])
			for (var i = 0; i < ordered.length; i++) {
				var entry = Object.assign({}, ordered[i])
				entry.hidden = hidden(entry.name)
				out.push(entry)
			}
		}
		return out
	}
}
