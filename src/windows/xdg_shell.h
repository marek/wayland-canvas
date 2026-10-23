#pragma once

#include "core/util.h"

class Server;

// The xdg-shell global: every toplevel a client makes through it becomes a
// view of the server, and every popup a popup.
class XdgShell {
public:
	explicit XdgShell(Server &server) : server_(server) {}
	XdgShell(const XdgShell &) = delete;
	XdgShell &operator=(const XdgShell &) = delete;

	// Creates the global; needs the Wayland display.
	void init();
	// Stops listening, before the display goes.
	void finish();

private:
	void handle_new_toplevel(wlr_xdg_toplevel *toplevel);
	void handle_new_popup(wlr_xdg_popup *popup);

	Server &server_;
	Listener new_toplevel_, new_popup_;
};
