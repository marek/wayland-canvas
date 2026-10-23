import QtQuick

// api.shell (docs/intents/shell/plugins.md): the shell config, live, and
// the writes the host makes of it. It offers exactly these members: the
// shell's own parts behind it are not the API, so a plugin cannot write the
// config over the host's head or take another plugin's api.
QtObject {
	id: root

	// ShellConfig and Contributions.
	required property var _config
	required property var _contributions

	readonly property var config: _config.data
	readonly property string configPath: _config.path
	readonly property var barWidgets: _contributions.barWidgets
	readonly property var toolbarButtons: _contributions.toolbarButtons

	// The toolbar's edge, live from the config; "right" when unset.
	readonly property string toolbarPosition: {
		var position = config.toolbar ? config.toolbar.position : undefined
		return isEdge(position) ? position : "right"
	}

	// The app bar's edge and switch, live from the config: the left edge and
	// on when unset (docs/intents/shell/app-bar.md).
	readonly property string appBarPosition: {
		var position = config.appbar ? config.appbar.position : undefined
		return isEdge(position) ? position : "left"
	}
	readonly property bool appBarEnabled:
		!(config.appbar && config.appbar.enabled === false)

	// One page per plugin that declares settings the user may be asked
	// about, for the settings window to render
	// (docs/intents/shell/plugins.md): {id, name, groups: [{name, entries}]},
	// each entry {key, type, label, description, choices, min, max, step,
	// value}. A key without a type and a label is the plugin's own and
	// appears nowhere.
	readonly property var pluginSettingsPages: {
		var pages = []
		var plugins = _contributions.plugins
		for (var i = 0; i < plugins.length; i++) {
			var plugin = plugins[i]
			var pluginApi = plugin.api.plugin
			var schema = pluginApi.schema
			var groups = []
			var byGroup = ({})
			var keys = Object.keys(schema)
			for (var k = 0; k < keys.length; k++) {
				var key = keys[k]
				var spec = schema[key]
				var kind = spec.type || ""
				if (spec.label === undefined || (kind !== "switch"
						&& kind !== "choice" && kind !== "number" && kind !== "text")) {
					continue // the plugin's own: nothing to show
				}
				var name = spec.group !== undefined ? spec.group : plugin.manifest.name
				if (byGroup[name] === undefined) {
					byGroup[name] = {name: name, entries: []}
					groups.push(byGroup[name])
				}
				byGroup[name].entries.push({
					key: key,
					type: kind,
					label: spec.label,
					description: spec.description !== undefined ? spec.description : "",
					choices: spec.choices !== undefined ? spec.choices : [],
					min: spec.min !== undefined ? spec.min : 0,
					max: spec.max !== undefined ? spec.max : 100,
					step: spec.step !== undefined ? spec.step : 1,
					order: spec.order !== undefined ? spec.order : 0,
					value: pluginApi.settings[key],
				})
			}
			if (groups.length === 0) {
				continue
			}
			for (var g = 0; g < groups.length; g++) {
				groups[g].entries.sort(function(a, b) { return a.order - b.order })
			}
			pages.push({id: plugin.id, name: plugin.manifest.name, groups: groups})
		}
		return pages
	}

	function isEdge(edge) {
		return edge === "right" || edge === "left" || edge === "top" || edge === "bottom"
	}

	// Whether the config's bar.hidden names this widget.
	function barHidden(name) {
		return _contributions.hidden(name)
	}

	// Adds or removes one widget from bar.hidden. The bar loses or
	// regains the widget at once — the bar widgets are ordered again,
	// never the whole shell reloaded.
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
		_config.set(["bar", "hidden"], list)
		_contributions.reorder()
		return true
	}

	// Writes toolbar.position; the toolbar follows the live
	// toolbarPosition binding. Anything but an edge is refused.
	function setToolbarPosition(edge) {
		if (!isEdge(edge)) {
			return false
		}
		_config.set(["toolbar", "position"], edge)
		return true
	}

	// Writes appbar.position and appbar.enabled; both bars follow the live
	// bindings. Anything but an edge is refused.
	function setAppBarPosition(edge) {
		if (!isEdge(edge)) {
			return false
		}
		_config.set(["appbar", "position"], edge)
		return true
	}

	function setAppBarEnabled(on) {
		_config.set(["appbar", "enabled"], on === true)
		return true
	}

	// Writes one plugin's setting, through that plugin's own api: the live
	// settings change at once and the override lands in the config.
	function setPluginSetting(id, key, value) {
		var plugins = _contributions.plugins
		for (var i = 0; i < plugins.length; i++) {
			if (plugins[i].id === id) {
				plugins[i].api.plugin.setSetting(key, value)
				return true
			}
		}
		return false
	}
}
