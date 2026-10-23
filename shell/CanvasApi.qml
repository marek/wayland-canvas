import QtQuick

// api.canvas: a plugin's own surface on the plane
// (docs/intents/platform/ipc.md). The plugin draws it; the compositor is told
// only which surface and which canvas rectangle, and pans, zooms and scales
// it with everything else on the plane.
QtObject {
	id: root

	property var compositor: null

	function place(name, box, callback) {
		compositor.call("surface.place", {
			surface: name,
			x: Math.round(box.x),
			y: Math.round(box.y),
			width: Math.round(box.width),
			height: Math.round(box.height)
		}, callback)
	}

	function unplace(name, callback) {
		compositor.call("surface.unplace", {surface: name}, callback)
	}

	// From a press on the surface: the compositor moves it with the
	// pointer, and callback(box, error) hears the rectangle it was dropped
	// at when the button is released.
	function move(name, callback) {
		compositor.call("surface.move", {surface: name}, callback)
	}

	// Likewise sized by its edges (["right", "bottom"]), never below
	// minWidth × minHeight.
	function resize(name, options, callback) {
		compositor.call("surface.resize", {
			surface: name,
			edges: options.edges,
			minWidth: Math.round(options.minWidth || 1),
			minHeight: Math.round(options.minHeight || 1)
		}, callback)
	}
}
