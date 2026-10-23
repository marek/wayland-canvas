#pragma once

#include "core/util.h"

class Server;

// The layer-shell global: a surface a client makes through it goes to the
// output it names, else the one under the pointer.
class LayerShell {
public:
	explicit LayerShell(Server &server) : server_(server) {}
	LayerShell(const LayerShell &) = delete;
	LayerShell &operator=(const LayerShell &) = delete;

	// Creates the global, logging when it cannot be made; needs the Wayland
	// display.
	void init();
	// Stops listening, before the display goes.
	void finish();

	// nullptr when the global could not be made.
	wlr_layer_shell_v1 *wlr() const { return wlr_; }

private:
	void handle_new_surface(wlr_layer_surface_v1 *layer_surface);

	Server &server_;
	wlr_layer_shell_v1 *wlr_ = nullptr;
	Listener new_surface_;
};
