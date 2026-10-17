// The X11 half of View, and the Xwayland server behind it: everything an
// X11 window answers differently from a wayland one. Its twin is
// view_xdg.cpp; the canvas logic they share is view.cpp.
//
// An X11 window is a view like any other — title bar, desktop, overview,
// minimize, maximize, fullscreen, close (docs/intents/windows/x11.md).
#include <atomic>
#include "core/server.h"
#include "platform/output.h"
#include "windows/view.h"

#if CANVAS_HAVE_XWAYLAND

View::View(Server &server, wlr_xwayland_surface *x11)
	: server(server), x11(x11), deco(*this) {
	static std::atomic<uint64_t> next_session_id = 1;
	session_id = next_session_id++;
	listen_x11();
}

void View::listen_x11() {
	x11->data = this;
	// An X11 window exists before it has a wayland surface: the pair is
	// made and broken by associate and dissociate, and only then are the
	// surface's own events there to listen to.
	x11_associate_.connect<&View::handle_x11_associate>(x11->events.associate, this);
	x11_dissociate_.connect<&View::handle_x11_dissociate>(x11->events.dissociate, this);
	destroy_.connect<&View::handle_destroy>(x11->events.destroy, this);
	request_move_.connect<&View::handle_request_move>(x11->events.request_move, this);
	x11_configure_.connect<&View::handle_x11_configure>(x11->events.request_configure, this);
	request_maximize_.connect<&View::handle_x11_request_maximize>(
		x11->events.request_maximize, this);
	request_minimize_.connect<&View::handle_x11_request_minimize>(
		x11->events.request_minimize, this);
	request_fullscreen_.connect<&View::handle_x11_request_fullscreen>(
		x11->events.request_fullscreen, this);
	set_title_.connect<&View::handle_set_title>(x11->events.set_title, this);
	set_app_id_.connect<&View::handle_set_app_id>(x11->events.set_class, this);
}

// ---- what the shared code asks ----

wlr_surface *View::x11_surface() const {
	return x11->surface;
}

wlr_box View::x11_geometry() const {
	// X11 has no window geometry of its own: the window is its surface.
	if (x11->surface != nullptr && x11->surface->mapped) {
		return wlr_box{0, 0, x11->surface->current.width,
			x11->surface->current.height};
	}
	return wlr_box{0, 0, x11->width, x11->height};
}

bool View::x11_initialized() const {
	return x11->surface != nullptr;
}

void View::x11_send_close() {
	wlr_xwayland_surface_close(x11);
}

void View::x11_configure_size(int width, int height) {
	if (width <= 0 || height <= 0) {
		return; // X11 has no "your own size": the window keeps what it has
	}
	wlr_xwayland_surface_configure(x11, static_cast<int16_t>(x),
		static_cast<int16_t>(y), static_cast<uint16_t>(width),
		static_cast<uint16_t>(height));
}

void View::x11_tell_maximized(bool want) {
	wlr_xwayland_surface_set_maximized(x11, want, want);
}

void View::x11_tell_fullscreen(bool want) {
	wlr_xwayland_surface_set_fullscreen(x11, want);
}

void View::x11_tell_tiled(bool) {
	// X11 has no tiled state to tell; the slice's size is the configure.
}

void View::x11_tell_activated(bool want) {
	wlr_xwayland_surface_activate(x11, want);
}

const char *View::x11_title() const {
	return x11->title;
}

const char *View::x11_app_id() const {
	return x11->class_;
}

pid_t View::x11_client_pid() const {
	return static_cast<pid_t>(x11->pid);
}

bool View::x11_wants_maximized() const {
	return x11->maximized_horz && x11->maximized_vert;
}

bool View::x11_wants_fullscreen() const {
	return x11->fullscreen;
}

bool View::x11_resize_settled(uint32_t) const {
	return true; // X11 acks nothing: the resize ends with the button
}

void View::x11_for_each_surface(
		std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const {
	if (x11->surface == nullptr) {
		return;
	}
	wlr_surface_for_each_surface(
		x11->surface,
		[](wlr_surface *surface, int sx, int sy, void *data) {
			auto &callback =
				*static_cast<std::function_ref<void(wlr_surface *, int, int)> *>(data);
			callback(surface, sx, sy);
		},
		&fn);
}

wlr_surface *View::x11_surface_at(double lx, double ly, double *sx, double *sy) const {
	if (x11->surface == nullptr) {
		return nullptr;
	}
	return wlr_surface_surface_at(x11->surface, lx, ly, sx, sy);
}

View *View::from_x11_surface(wlr_surface *surface) {
	wlr_xwayland_surface *xwl = wlr_xwayland_surface_try_from_wlr_surface(
		wlr_surface_get_root_surface(surface));
	return xwl != nullptr ? static_cast<View *>(xwl->data) : nullptr;
}

// ---- the X11 window's own events ----

void View::handle_x11_associate() {
	// The window now has a surface: from here it behaves as any view does.
	map_.connect<&View::handle_map>(x11->surface->events.map, this);
	unmap_.connect<&View::handle_unmap>(x11->surface->events.unmap, this);
	commit_.connect<&View::handle_commit>(x11->surface->events.commit, this);
	handle_first_commit();
}

void View::handle_x11_dissociate() {
	map_.disconnect();
	unmap_.disconnect();
	commit_.disconnect();
}

void View::handle_x11_configure(wlr_xwayland_surface_configure_event *event) {
	// A window the compositor is not placing itself gets the size it asks
	// for, where the canvas has put it.
	if (maximized || fullscreen || tiled) {
		configure_size(shell_geometry().width, shell_geometry().height);
		return;
	}
	wlr_xwayland_surface_configure(x11, static_cast<int16_t>(x),
		static_cast<int16_t>(y), event->width, event->height);
	damage();
}

void View::handle_x11_request_maximize() {
	set_maximized(x11->maximized_horz && x11->maximized_vert);
}

void View::handle_x11_request_minimize() {
	set_minimized(x11->minimized);
}

void View::handle_x11_request_fullscreen() {
	set_fullscreen(x11->fullscreen);
}

// ---- the Xwayland server ----

void Server::init_xwayland() {
	// Lazy: Xwayland starts with the first X11 client, not at login.
	xwayland = wlr_xwayland_create(display, compositor, true);
	if (xwayland == nullptr) {
		wlr_log(WLR_INFO, "Xwayland is not available: this session has no X11");
		return;
	}
	new_xwayland_surface_.connect<&Server::handle_new_xwayland_surface>(
		xwayland->events.new_surface, this);
	xwayland_ready_.connect<&Server::handle_xwayland_ready>(
		xwayland->events.ready, this);
	wlr_xwayland_set_seat(xwayland, seat);
	setenv("DISPLAY", xwayland->display_name, true);
}

void Server::finish_xwayland() {
	new_xwayland_surface_.disconnect();
	xwayland_ready_.disconnect();
	if (xwayland != nullptr) {
		wlr_xwayland_destroy(xwayland);
		xwayland = nullptr;
	}
	unsetenv("DISPLAY");
}

void Server::handle_xwayland_ready() {
	// The clipboard goes both ways from here on: wlroots bridges the X11
	// selection to the seat's.
	wlr_xwayland_set_seat(xwayland, seat);
}

void Server::handle_new_xwayland_surface(wlr_xwayland_surface *surface) {
	toplevels.push_back(std::make_unique<View>(*this, surface));
}

#else // CANVAS_HAVE_XWAYLAND

// Built without Xwayland: the compositor runs, and a machine with no
// Xwayland simply has no X11 (docs/intents/windows/x11.md).
void Server::init_xwayland() {
	wlr_log(WLR_INFO, "built without Xwayland: this session has no X11");
}

void Server::finish_xwayland() {}
void Server::handle_xwayland_ready() {}

View *View::from_x11_surface(wlr_surface *) {
	return nullptr;
}

#endif
