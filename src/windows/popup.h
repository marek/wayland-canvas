#pragma once

#include "core/util.h"

class Server;

// An xdg popup; its surface tree belongs to its parent's.
class Popup {
public:
	Popup(Server &server, wlr_xdg_popup *wlr);
	~Popup();
	Popup(const Popup &) = delete;
	Popup &operator=(const Popup &) = delete;

	wlr_xdg_popup *const wlr;

private:
	void handle_commit();
	void handle_destroy();

	Server &server_;
	Listener commit_, destroy_;
};
