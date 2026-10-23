import Quickshell
import Quickshell.Io
import QtQuick

// The plugin host (docs/intents/shell/plugins.md): six parts and what joins
// them. ShellConfig is shell.json and where the shell's files live;
// PluginDiscovery finds the plugins in the built-in, user and configured
// directories — a plugin in a later one replaces one with the same id in an
// earlier one — reads their manifests and skips what cannot load, with a
// line saying why; Contributions takes what the plugins contribute, each
// whole or not at all; ShellApi is api.shell; SurfaceHost makes the
// surfaces, one per output where a manifest asks; ReloadWatcher reloads the
// whole shell on an edit to the config or any plugin's files. The host
// hands every contributed component one api: plugin, storage, panels,
// canvas, actions, theme, compositor, shell. It draws nothing itself:
// everything on screen belongs to a plugin.
Item {
	id: host

	// The plugin API this host offers; a manifest asking for more is not
	// loaded.
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

		// A script's or keybinding's action.run arrives as an event: the
		// host runs the named plugin action, exactly as a toolbar button
		// would.
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

	// Gives each plugin that can load its api, then takes what they
	// contribute and brings the shell up: the theme first, so the surfaces
	// are drawn in it from their first frame.
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
