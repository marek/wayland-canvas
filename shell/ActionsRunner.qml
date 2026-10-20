import QtQuick

// api.actions: run("<plugin id>/<action id>", args) — a manifest's action
// entry is a component whose run(args) performs it; it is instantiated with
// its plugin's api and destroyed once run has returned, so an action is a
// synchronous burst of calls (processes it wants to leave running go
// through Quickshell.execDetached).
Item {
	id: root

	property var actions: ({}) // global name -> {component, api}
	property var warned: ({}) // names already logged as missing

	// The host registers each manifest action entry here.
	function register(name, component, api) {
		actions[name] = {component: component, api: api}
	}

	function run(name, args) {
		var entry = actions[name]
		if (entry == null) {
			if (!warned[name]) {
				warned[name] = true
				console.warn("[plugins] no action named " + name)
			}
			return
		}
		var instance = entry.component.createObject(this, {api: entry.api})
		if (instance == null) {
			return
		}
		instance.run(args)
		instance.destroy()
	}
}
