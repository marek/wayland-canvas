pragma Singleton

import QtQuick

// The dialog's stand-in flow: what a scenario drives instead of polkitd,
// because a test must never register an agent with the machine's own
// polkitd (docs/intents/shell/password-dialog.md). It answers exactly what
// an AuthFlow answers, and records every step in the plugin's storage so a
// scenario can read what the dialog did.
Item {
	id: root

	property var api: null
	property var flow: null

	readonly property string storageName: "test-flow.json"

	function init(pluginApi) {
		if (api === null) {
			api = pluginApi
		}
	}

	function record(what, detail) {
		if (api === null) {
			return
		}
		var log = api.storage.read(storageName)
		if (log == null) {
			log = []
		}
		log.push({step: what, detail: detail === undefined ? "" : detail})
		api.storage.write(storageName, log)
	}

	function start(args) {
		var expected = args != null && args.password !== undefined
			? String(args.password) : ""
		var message = args != null && args.message !== undefined
			? String(args.message) : "An application is asking for admin rights"
		var user = args != null && args.user !== undefined ? String(args.user) : ""
		flow = standin.createObject(root, {
			message: message,
			expected: expected,
			identityName: user
		})
		record("started", message)
	}

	function stop() {
		if (flow !== null) {
			flow.destroy()
			flow = null
		}
	}

	Component {
		id: standin

		QtObject {
			property string message: ""
			property string expected: ""
			property string identityName: ""
			property bool isResponseRequired: true
			property string inputPrompt: "Password:"
			property bool responseVisible: false
			property string supplementaryMessage: ""
			property bool supplementaryIsError: false
			property var selectedIdentity: identityName === ""
				? null : ({userName: identityName})

			function submit(answer) {
				if (String(answer) === expected) {
					root.record("authenticated")
					root.stop()
					return
				}
				supplementaryMessage = "Wrong password"
				supplementaryIsError = true
				root.record("refused", String(answer).length)
			}

			function cancelAuthenticationRequest() {
				root.record("cancelled")
				root.stop()
			}
		}
	}
}
