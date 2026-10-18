#pragma once

#include <optional>
#include <string>
#include "core/util.h"

class Output;
class Server;

// A layer-shell surface (bar, wallpaper, notification) on one output.
class LayerSurface {
public:
	LayerSurface(Server &server, Output &output, wlr_layer_surface_v1 *wlr);
	~LayerSurface();
	LayerSurface(const LayerSurface &) = delete;
	LayerSurface &operator=(const LayerSurface &) = delete;

	Server &server;
	Output &output;
	wlr_layer_surface_v1 *const wlr;
	wlr_box geo{}; // output-local logical
	bool mapped = false;
	// A surface a plugin asked to be put on the plane
	// (docs/intents/platform/ipc.md): its rectangle in canvas pixels. It
	// is then drawn, moved and hit-tested with the canvas instead of with
	// the screen. Unset for an ordinary layer surface.
	std::optional<wlr_box> canvas_box;
	// The name the plugin placed it by: its layer-shell namespace.
	std::string name() const;

	// Keyboard interactivity, as the client last committed it.
	bool wants_keyboard() const;
	bool holds_keyboard_exclusively() const;

private:
	void handle_map();
	void handle_unmap();
	void handle_commit();
	void handle_destroy();

	// False once the owning output is destroying the wlroots object: map,
	// unmap, commit and destroy must not arrange on it any more.
	bool attached_ = true;

	friend class Output;

	Listener map_, unmap_, commit_, destroy_;
};

// The surface under an output-local point on a layer, with surface-local coordinates.
struct LayerHit {
	LayerSurface *layer = nullptr;
	wlr_surface *surface = nullptr;
	double sx = 0, sy = 0;
};
