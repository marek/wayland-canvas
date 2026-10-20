import QtQuick

// api.shell (docs/intents/shell/plugins.md): the shell config, live, and
// the writes the host makes of it. It offers exactly these members: the
// host itself is not reachable through it, so a plugin cannot write the
// config over the host's head or take another plugin's api.
QtObject {
	id: root

	required property var _host

	readonly property var config: _host.config
	readonly property string configPath: _host.configPath
	readonly property var barWidgets: _host.barWidgets
	readonly property string toolbarPosition: _host.toolbarPosition
	readonly property string appBarPosition: _host.appBarPosition
	readonly property bool appBarEnabled: _host.appBarEnabled
	readonly property var pluginSettingsPages: _host.pluginSettingsPages

	function barHidden(name) {
		return _host.barHidden(name)
	}

	function setBarHidden(name, hidden) {
		return _host.setBarHidden(name, hidden)
	}

	function setToolbarPosition(edge) {
		return _host.setToolbarPosition(edge)
	}

	function setAppBarPosition(edge) {
		return _host.setAppBarPosition(edge)
	}

	function setAppBarEnabled(on) {
		return _host.setAppBarEnabled(on)
	}

	function setPluginSetting(id, key, value) {
		return _host.setPluginSetting(id, key, value)
	}
}
