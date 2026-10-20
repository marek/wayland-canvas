// The xdg-shell half of View: everything a wayland window answers that an
// X11 one answers differently, and the xdg-shell globals that bring the
// windows. Its twin is view_x11.cpp; the canvas logic they share is view.cpp,
// which names none of these protocols.
#include "core/server.h"
#include "platform/output.h"
#include "windows/popup.h"
#include "windows/view_xdg.h"

XdgView::XdgView(Server &server, wlr_xdg_toplevel *wlr)
	: View(server), wlr(wlr) {
	wlr->base->data = static_cast<View *>(this);
	map_.connect<&XdgView::handle_map>(wlr->base->surface->events.map, this);
	unmap_.connect<&XdgView::handle_unmap>(wlr->base->surface->events.unmap, this);
	commit_.connect<&XdgView::handle_xdg_commit>(wlr->base->surface->events.commit, this);
	destroy_.connect<&XdgView::handle_destroy>(wlr->events.destroy, this);
	request_move_.connect<&XdgView::handle_request_move>(wlr->events.request_move, this);
	request_resize_.connect<&XdgView::handle_xdg_request_resize>(
		wlr->events.request_resize, this);
	request_maximize_.connect<&XdgView::handle_xdg_request_maximize>(
		wlr->events.request_maximize, this);
	request_minimize_.connect<&XdgView::handle_xdg_request_minimize>(
		wlr->events.request_minimize, this);
	request_fullscreen_.connect<&XdgView::handle_xdg_request_fullscreen>(
		wlr->events.request_fullscreen, this);
	set_title_.connect<&XdgView::handle_set_title>(wlr->events.set_title, this);
	set_app_id_.connect<&XdgView::handle_set_app_id>(wlr->events.set_app_id, this);
}

// ---- what the shared code asks ----

wlr_surface *XdgView::surface() const {
	return wlr->base->surface;
}

wlr_box XdgView::shell_geometry() const {
	return wlr->base->geometry;
}

bool XdgView::initialized() const {
	return wlr->base->initialized;
}

void XdgView::send_close() {
	wlr_xdg_toplevel_send_close(wlr);
}

void XdgView::configure_size(int width, int height) {
	wlr_xdg_toplevel_set_size(wlr, width, height);
}

void XdgView::tell_maximized(bool want) {
	wlr_xdg_toplevel_set_maximized(wlr, want);
}

void XdgView::tell_fullscreen(bool want) {
	wlr_xdg_toplevel_set_fullscreen(wlr, want);
}

void XdgView::tell_tiled(bool want) {
	wlr_xdg_toplevel_set_tiled(wlr, want
		? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
		: 0);
}

void XdgView::tell_activated(bool want) {
	wlr_xdg_toplevel_set_activated(wlr, want);
}

const char *XdgView::shell_title() const {
	return wlr->title;
}

const char *XdgView::shell_app_id() const {
	return wlr->app_id;
}

pid_t XdgView::client_pid() const {
	pid_t pid = 0;
	wl_client_get_credentials(wl_resource_get_client(wlr->resource), &pid, nullptr,
		nullptr);
	return pid;
}

bool XdgView::wants_maximized() const {
	return wlr->requested.maximized;
}

bool XdgView::wants_fullscreen() const {
	return wlr->requested.fullscreen;
}

View *XdgView::shell_parent() const {
	if (wlr->parent == nullptr || wlr->parent->base == nullptr) {
		return nullptr;
	}
	return static_cast<View *>(wlr->parent->base->data);
}

// Modal through xdg-dialog-v1: the client holds its parent's input until it goes.
bool XdgView::modal() const {
	wlr_xdg_dialog_v1 *dialog = wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(wlr);
	return dialog != nullptr && dialog->modal;
}

bool XdgView::client_placed() const {
	return false; // an xdg toplevel is placed by the compositor
}

bool XdgView::decorated_without_negotiating() const {
	return false; // xdg-decoration or the KDE protocol decides (decoration.cpp)
}

SizeLimit XdgView::min_size() const {
	return {wlr->current.min_width, wlr->current.min_height};
}

SizeLimit XdgView::max_size() const {
	return {wlr->current.max_width, wlr->current.max_height};
}

void XdgView::for_each_surface(
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

wlr_surface *XdgView::surface_at(double lx, double ly, double *sx, double *sy) const {
	return wlr_xdg_surface_surface_at(wlr->base, lx, ly, sx, sy);
}

XdgView *XdgView::from_surface(wlr_surface *surface) {
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
	return static_cast<XdgView *>(static_cast<View *>(toplevel->base->data));
}

// ---- an interactive resize ----

void XdgView::begin_interactive_resize(uint32_t edges) {
	resize_anchor_ = ResizeAnchor{.edges = edges, .start = content_box()};
	wlr_xdg_toplevel_set_resizing(wlr, true);
}

void XdgView::resize_to(wlr_box content) {
	// The size only: the client's commit moves the window to keep the
	// opposite edges where they were (anchor_resize).
	wlr_xdg_toplevel_set_size(wlr, content.width, content.height);
}

void XdgView::end_interactive_resize() {
	if (!resize_anchor_.has_value() || resize_anchor_->end_serial != 0) {
		return;
	}
	resize_anchor_->end_serial = wlr_xdg_toplevel_set_resizing(wlr, false);
}

void XdgView::drop_resize() {
	resize_anchor_.reset();
}

void XdgView::anchor_resize() {
	if (!mapped || !resize_anchor_.has_value()) {
		return;
	}
	wlr_box geo = shell_geometry();
	const wlr_box &start = resize_anchor_->start;
	if (resize_anchor_->edges & WLR_EDGE_LEFT) {
		x = start.x + start.width - geo.width;
	}
	if (resize_anchor_->edges & WLR_EDGE_TOP) {
		y = start.y + start.height - geo.height;
	}
	if (resize_anchor_->end_serial != 0
			&& wlr->base->current.configure_serial >= resize_anchor_->end_serial) {
		resize_anchor_.reset();
	}
}

// ---- the xdg shell's own events ----

void XdgView::handle_xdg_commit() {
	if (wlr->base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(wlr,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		handle_first_commit();
		return;
	}
	anchor_resize();
	handle_commit();
}

void XdgView::handle_xdg_request_resize(wlr_xdg_toplevel_resize_event *event) {
	handle_request_resize(event->edges);
}

void XdgView::handle_xdg_request_maximize() {
	if (!initialized()) {
		return; // the initial commit will honour the request
	}
	set_maximized(wlr->requested.maximized);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

void XdgView::handle_xdg_request_minimize() {
	if (!initialized()) {
		return;
	}
	set_minimized(wlr->requested.minimized);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

void XdgView::handle_xdg_request_fullscreen() {
	if (!initialized()) {
		return;
	}
	set_fullscreen(wlr->requested.fullscreen);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

// ---- the xdg-shell globals ----

void Server::init_xdg_shell() {
	xdg_shell = wlr_xdg_shell_create(display, 6);
	new_xdg_toplevel_.connect<&Server::handle_new_xdg_toplevel>(
		xdg_shell->events.new_toplevel, this);
	new_xdg_popup_.connect<&Server::handle_new_xdg_popup>(
		xdg_shell->events.new_popup, this);
}

void Server::finish_xdg_shell() {
	new_xdg_toplevel_.disconnect();
	new_xdg_popup_.disconnect();
}

void Server::handle_new_xdg_toplevel(wlr_xdg_toplevel *toplevel) {
	toplevels.push_back(std::make_unique<XdgView>(*this, toplevel));
}

void Server::handle_new_xdg_popup(wlr_xdg_popup *popup) {
	popups.push_back(std::make_unique<Popup>(*this, popup));
}
