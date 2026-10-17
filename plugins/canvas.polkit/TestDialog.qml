import QtQuick
import "."

// canvas.polkit/test-dialog: opens the dialog on a stand-in flow, the one
// way a scenario sees its states without a polkitd.
Item {
	property var api: null

	function run(args) {
		PolkitState.init(api)
		PolkitState.start(args)
	}
}
