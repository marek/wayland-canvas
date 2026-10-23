import Quickshell
import Quickshell.Io
import QtQuick

// The plugin host (docs/intents/shell/plugins.md): wires the parts below and
// hands every contributed component one api (plugin, storage, panels, canvas,
// actions, theme, compositor, shell). It draws nothing itself.
Item {
	id: host

	// A manifest asking for a higher apiVersion is not loaded.
	readonly property int apiVersion: 1

	ShellConfig {
		id: shellConfig
		onEdited: reloadWatcher.request()
	}

	PluginDiscovery {
		id: discovery
		apiVersion: host.apiVersion
		config: shellConfig.data
		onFound: function(plugins) {
			host.load(plugins)
		}
	}

	Contributions {
		id: contributions
		panelsManager: panels
		actionsRunner: actions
		config: shellConfig
	}

	ShellApi {
		id: shellApi
		_config: shellConfig
		_contributions: contributions
	}

	SurfaceHost {
		id: surfaceHost
	}

	ReloadWatcher {
		id: reloadWatcher
		configPath: shellConfig.path
	}

	Compositor {
		id: compositor

		// A script's or keybinding's action.run runs the plugin action as a
		// toolbar button does.
		onEvent: function(name, params) {
			if (name === "action") {
				actions.run(params.name, params.args)
			}
		}
	}

	PanelsManager {
		id: panels
	}

	CanvasApi {
		id: canvasApi
		compositor: compositor
	}

	ActionsRunner {
		id: actions
	}

	ThemeApi {
		id: theme
		_config: shellConfig
		_compositor: compositor
	}

	Component {
		id: pluginApiComponent
		PluginApi {}
	}

	Component {
		id: storageApiComponent
		StorageApi {}
	}

	Process {
		id: mkdir
	}

	Component.onCompleted: {
		shellConfig.load()
		discovery.discover(shellConfig.pluginRoots())
	}

	// A plugin's own setSetting landing in the config as the plugin's
	// plugins.<id>.settings override.
	function writePluginSetting(pluginId, key, value) {
		shellConfig.set(["plugins", pluginId, "settings", key], value)
	}

	// Gives each loadable plugin its api and takes what they contribute; the
	// theme starts before the surfaces exist, so they draw in it from the
	// first frame.
	function load(found) {
		var loaded = []
		var storageDirs = []
		for (var i = 0; i < found.length; i++) {
			var plugin = found[i]
			var own = shellConfig.data.plugins ? shellConfig.data.plugins[plugin.id] : null
			var pluginApi = pluginApiComponent.createObject(host, {
				pluginId: plugin.id,
				manifest: plugin.manifest,
				overrides: own ? own.settings : null,
			})
			pluginApi.settingWritten.connect(writePluginSetting)
			var storage = storageApiComponent.createObject(host,
				{dir: shellConfig.stateDir + "/" + plugin.id})
			storageDirs.push(storage.dir)
			loaded.push({
				id: plugin.id,
				dir: plugin.dir,
				manifest: plugin.manifest,
				api: {
					plugin: pluginApi,
					storage: storage,
					panels: panels,
					canvas: canvasApi,
					actions: actions,
					theme: theme,
					compositor: compositor,
					shell: shellApi,
				},
			})
		}
		// Each plugin's storage directory appears when its api does.
		if (storageDirs.length > 0) {
			mkdir.command = ["mkdir", "-p", "--"].concat(storageDirs)
			mkdir.running = true
		}
		contributions.load(loaded)
		theme.start()
		surfaceHost.entries = contributions.surfaces
		reloadWatcher.watch(shellConfig.pluginRoots().concat([Quickshell.shellPath("")]))
	}
}
