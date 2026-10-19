// The xdg-shell half of View: everything a wayland window answers that an
// X11 one answers differently. Its twin is view_x11.cpp; the canvas logic
// they share is view.cpp, which names none of these protocols.
#include <atomic>
#include "core/server.h"
#include "platform/output.h"
#include "windows/view.h"

View::View(Server &server, wlr_xdg_toplevel *xdg)
	: server(server), wlr(xdg), deco(*this) {
	static std::atomic<uint64_t> next_session_id = 1;
	session_id = next_session_id++;
	listen_xdg();
}

void View::listen_xdg() {
	wlr->base->data = this;
	map_.connect<&View::handle_map>(wlr->base->surface->events.map, this);
	unmap_.connect<&View::handle_unmap>(wlr->base->surface->events.unmap, this);
	commit_.connect<&View::handle_xdg_commit>(wlr->base->surface->events.commit, this);
	destroy_.connect<&View::handle_destroy>(wlr->events.destroy, this);
	request_move_.connect<&View::handle_request_move>(wlr->events.request_move, this);
	request_resize_.connect<&View::handle_request_resize>(
		wlr->events.request_resize, this);
	request_maximize_.connect<&View::handle_xdg_request_maximize>(
		wlr->events.request_maximize, this);
	request_minimize_.connect<&View::handle_xdg_request_minimize>(
		wlr->events.request_minimize, this);
	request_fullscreen_.connect<&View::handle_xdg_request_fullscreen>(
		wlr->events.request_fullscreen, this);
	set_title_.connect<&View::handle_set_title>(wlr->events.set_title, this);
	set_app_id_.connect<&View::handle_set_app_id>(wlr->events.set_app_id, this);
}

// ---- what the shared code asks ----

wlr_surface *View::xdg_surface() const {
	return wlr->base->surface;
}

wlr_box View::xdg_geometry() const {
	return wlr->base->geometry;
}

bool View::xdg_initialized() const {
	return wlr->base->initialized;
}

void View::xdg_send_close() {
	wlr_xdg_toplevel_send_close(wlr);
}

void View::xdg_configure_size(int width, int height) {
	wlr_xdg_toplevel_set_size(wlr, width, height);
}

void View::xdg_tell_maximized(bool want) {
	wlr_xdg_toplevel_set_maximized(wlr, want);
}

void View::xdg_tell_fullscreen(bool want) {
	wlr_xdg_toplevel_set_fullscreen(wlr, want);
}

void View::xdg_tell_tiled(bool want) {
	wlr_xdg_toplevel_set_tiled(wlr, want
		? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
		: 0);
}

void View::xdg_tell_activated(bool want) {
	wlr_xdg_toplevel_set_activated(wlr, want);
}

const char *View::xdg_title() const {
	return wlr->title;
}

const char *View::xdg_app_id() const {
	return wlr->app_id;
}

pid_t View::xdg_client_pid() const {
	pid_t pid = 0;
	wl_client_get_credentials(wl_resource_get_client(wlr->resource), &pid, nullptr,
		nullptr);
	return pid;
}

bool View::xdg_wants_maximized() const {
	return wlr->requested.maximized;
}

bool View::xdg_wants_fullscreen() const {
	return wlr->requested.fullscreen;
}

View *View::xdg_parent() const {
	if (wlr->parent == nullptr || wlr->parent->base == nullptr) {
		return nullptr;
	}
	return static_cast<View *>(wlr->parent->base->data);
}

// Modal through xdg-dialog-v1: the client holds its parent's input until it goes.
bool View::xdg_modal() const {
	wlr_xdg_dialog_v1 *dialog = wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(wlr);
	return dialog != nullptr && dialog->modal;
}

bool View::xdg_resize_settled(uint32_t serial) const {
	return wlr->base->current.configure_serial >= serial;
}

void View::xdg_for_each_surface(
		std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const {
	wlr_xdg_surface_for_each_surface(
		wlr->base,
		[](wlr_surface *surface, int sx, int sy, void *data) {
			auto &callback =
				*static_cast<std::function_ref<void(wlr_surface *, int, int)> *>(data);
			callback(surface, sx, sy);
		},
		&fn);
}

wlr_surface *View::xdg_surface_at(double lx, double ly, double *sx, double *sy) const {
	return wlr_xdg_surface_surface_at(wlr->base, lx, ly, sx, sy);
}

View *View::from_xdg_surface(wlr_surface *surface) {
	surface = wlr_surface_get_root_surface(surface);
	while (true) {
		wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
		if (popup == nullptr || popup->parent == nullptr) {
			break;
		}
		surface = wlr_surface_get_root_surface(popup->parent);
	}
	wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);
	if (toplevel == nullptr || toplevel->base == nullptr) {
		return nullptr;
	}
	return static_cast<View *>(toplevel->base->data);
}

// ---- the xdg shell's own events ----

void View::handle_xdg_commit() {
	if (wlr->base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(wlr,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		handle_first_commit();
		return;
	}
	handle_commit();
}

void View::handle_xdg_request_maximize() {
	if (!initialized()) {
		return; // the initial commit will honour the request
	}
	set_maximized(wlr->requested.maximized);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

void View::handle_xdg_request_minimize() {
	if (!initialized()) {
		return;
	}
	set_minimized(wlr->requested.minimized);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

void View::handle_xdg_request_fullscreen() {
	if (!initialized()) {
		return;
	}
	set_fullscreen(wlr->requested.fullscreen);
	wlr_xdg_surface_schedule_configure(wlr->base);
}
