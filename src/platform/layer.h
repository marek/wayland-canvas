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
	// The output it was made for, or the one that took it over when that
	// output went (a surface on the plane outlives the output it began on).
	Output *output;
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
	// A surface whose name says it belongs on the plane. It is never
	// given a place on a screen: until the plugin says where on the
	// canvas it goes, it is not drawn at all — a surface meant for the
	// plane must never end up centred on the screen instead. Decided once,
	// when the surface is made: the namespace never changes, and pointer
	// motion asks for it.
	bool belongs_on_canvas() const;
	// Puts it at `box` on the plane, remembered under its name; the
	// client is told a new size, and every output showing it repaints.
	void place(wlr_box box);
	// Back to an ordinary layer surface on its screen; one whose name says
	// it belongs on the plane is drawn nowhere until placed again.
	void unplace();
	// Repaints the outputs that show it: on the plane, every output whose
	// view holds it; on a screen, its own.
	void damage();

	// Keyboard interactivity, as the client last committed it.
	bool wants_keyboard() const;
	bool holds_keyboard_exclusively() const;

	// Moves it onto `to`'s layers: its own output is going.
	void rehome(Output &to);

private:
	void handle_map();
	void handle_unmap();
	void handle_commit();
	void handle_destroy();

	// False once the owning output is destroying the wlroots object: map,
	// unmap, commit and destroy must not arrange on it any more.
	bool attached_ = true;
	const bool on_canvas_;

	friend class Output;

	Listener map_, unmap_, commit_, destroy_;
};

// The surface under an output-local point on a layer, with surface-local coordinates.
struct LayerHit {
	LayerSurface *layer = nullptr;
	wlr_surface *surface = nullptr;
	double sx = 0, sy = 0;
};
