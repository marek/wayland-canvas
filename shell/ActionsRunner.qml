import QtQuick

// api.actions: run("<plugin id>/<action id>", args) instantiates the action
// component with its plugin's api, calls run(args) and destroys it, so an
// action is a synchronous burst of calls (a process to leave running goes
// through Quickshell.execDetached).
Item {
	id: root

	property var actions: ({}) // global name -> {component, api}
	property var warned: ({}) // names already logged as missing

	// Contributions registers each manifest action entry here.
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
