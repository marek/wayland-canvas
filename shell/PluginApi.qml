import QtQuick

// api.plugin: the manifest, the settings (the manifest's defaults merged
// with the config's overrides), and setSetting, which writes the override
// back to the config.
Item {
	id: root

	property var host: null
	property string pluginId: ""
	property var manifest: null
	property var settings: ({})

	Component.onCompleted: mergeSettings()

	function mergeSettings() {
		var merged = {}
		var defaults = manifest != null && manifest.settings ? manifest.settings : {}
		for (var key in defaults) {
			merged[key] = defaults[key]
		}
		var overrides = host.config.plugins && host.config.plugins[pluginId]
			? host.config.plugins[pluginId].settings : null
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
		host.config.plugins = host.config.plugins || {}
		host.config.plugins[pluginId] = host.config.plugins[pluginId] || {}
		host.config.plugins[pluginId].settings = host.config.plugins[pluginId].settings || {}
		host.config.plugins[pluginId].settings[key] = value
		host.writeConfig()
	}
}
