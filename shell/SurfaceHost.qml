import Quickshell
import QtQuick

// Makes the plugins' surfaces: one of each, or one per screen where the
// manifest says perOutput. A screen that comes or goes makes or drops that
// screen's surfaces and touches no other's: Variants keeps an instance for
// every screen still there, with its widgets and what they hold.
Item {
	id: root

	// The surfaces to make: {component, api, perOutput}, as Contributions
	// lists them.
	property var entries: []

	// The models are positions in `entries`, not the entries: a model hands
	// its values to the delegates as variants, which would copy the objects.
	Instantiator {
		model: root.entries.length

		delegate: Variants {
			id: variants

			required property int index
			readonly property var entry: root.entries[index]

			// A surface that is not per-output is made once; a per-output
			// surface on each screen.
			model: entry.perOutput ? Quickshell.screens : [0]

			delegate: Item {
				id: seat

				// The screen, for a per-output surface.
				required property var modelData

				// The surface is the seat's child and goes with it; the seat
				// is an Item so a surface that is an Item has a parent. Only a
				// per-output surface is given its screen.
				Component.onCompleted: {
					var entry = variants.entry
					var properties = entry.perOutput
						? {api: entry.api, modelData: seat.modelData} : {api: entry.api}
					if (entry.component.createObject(seat, properties) == null) {
						console.warn("[plugins] a surface of "
							+ entry.api.plugin.manifest.id + " did not instantiate")
					}
				}
			}
		}
	}
}
