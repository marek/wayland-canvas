import QtQuick

// api.plugin: the manifest, the settings (the manifest's defaults merged
// with the config's overrides), and setSetting, which applies a value at
// once and hands it to the host to write back to the config.
//
// A manifest entry is the default value on its own, or an object carrying
// that default and what the settings window needs to render the setting —
// type, label, description, group, choices, range
// (docs/intents/shell/plugins.md). Either way `settings` holds values alone;
// the metadata is `schema`'s.
Item {
	id: root

	property string pluginId: ""
	property var manifest: null
	// plugins.<id>.settings of the config, when it has any.
	property var overrides: null
	property var settings: ({})
	// The declared metadata per key, for the keys that carry any: what the
	// settings window builds this plugin's page from.
	property var schema: ({})

	// A setSetting call, for the host to persist.
	signal settingWritten(string pluginId, string key, var value)

	Component.onCompleted: mergeSettings()

	// A declaration's default: the entry itself, or its `default` member
	// when the entry is a declaration object.
	function declaredDefault(entry) {
		return declaration(entry) != null ? entry.default : entry
	}

	// The entry as a declaration object, or null when it is a plain value.
	function declaration(entry) {
		return entry != null && typeof entry === "object" && !Array.isArray(entry)
			&& entry.default !== undefined ? entry : null
	}

	function mergeSettings() {
		var merged = {}
		var declared = {}
		var defaults = manifest != null && manifest.settings ? manifest.settings : {}
		for (var key in defaults) {
			merged[key] = declaredDefault(defaults[key])
			var spec = declaration(defaults[key])
			if (spec != null) {
				declared[key] = spec
			}
		}
		schema = declared
		if (overrides != null) {
			for (var name in overrides) {
				merged[name] = overrides[name]
			}
		}
		settings = merged
	}

	// Sets one setting: applied to the live settings at once (they are
	// re-merged from the config on the next reload) and persisted as a
	// plugins.<id>.settings override.
	function setSetting(key, value) {
		var merged = {}
		for (var name in settings) {
			merged[name] = settings[name]
		}
		merged[key] = value
		settings = merged
		settingWritten(pluginId, key, value)
	}
}
