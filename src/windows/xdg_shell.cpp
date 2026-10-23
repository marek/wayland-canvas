#include "windows/xdg_shell.h"

#include <memory>

#include "core/server.h"
#include "windows/popup.h"
#include "windows/view_xdg.h"

void XdgShell::init() {
	wlr_xdg_shell *shell = wlr_xdg_shell_create(server_.display, 6);
	new_toplevel_.connect<&XdgShell::handle_new_toplevel>(shell->events.new_toplevel, this);
	new_popup_.connect<&XdgShell::handle_new_popup>(shell->events.new_popup, this);
}

void XdgShell::finish() {
	new_toplevel_.disconnect();
	new_popup_.disconnect();
}

void XdgShell::handle_new_toplevel(wlr_xdg_toplevel *toplevel) {
	server_.toplevels.push_back(std::make_unique<XdgView>(server_, toplevel));
}

void XdgShell::handle_new_popup(wlr_xdg_popup *popup) {
	server_.popups.push_back(std::make_unique<Popup>(server_, popup));
}
