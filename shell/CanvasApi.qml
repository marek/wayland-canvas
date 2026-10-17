import QtQuick

// api.canvas: a plugin's own surface on the plane
// (docs/intents/platform/ipc.md). The plugin draws the surface itself —
// the compositor is told only which surface and which canvas rectangle,
// and from then on the plane pans, zooms and scales it like everything
// else on it.
QtObject {
	id: root

	property var compositor: null

	// The name a surface must carry for the compositor to find it: its
	// layer-shell namespace.
	function surfaceName(pluginId, surfaceId) {
		return "wayland-canvas-canvas-" + pluginId + "/" + surfaceId
	}

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
}
