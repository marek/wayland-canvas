// The X11 half of View, and the Xwayland server behind it: everything an
// X11 window answers differently from a wayland one. Its twin is
// view_xdg.cpp; the canvas logic they share is view.cpp.
//
// An X11 window is a view like any other — title bar, desktop, overview,
// minimize, maximize, fullscreen, close (docs/intents/windows/x11.md).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <vector>
#include "core/config.h"
#include "core/server.h"
#include "platform/output.h"
#include "windows/view.h"

#if CANVAS_HAVE_XWAYLAND

namespace {

// X positions are 16-bit: a window farther than that from the pointer waits
// at the limit (docs/intents/windows/x11.md).
int16_t x11_coordinate(int value) {
	return static_cast<int16_t>(std::clamp<int>(value, INT16_MIN, INT16_MAX));
}

// Whether X stacks the window: wlroots puts it in its stack on X's
// MapNotify, at the bottom, which can come after the surface maps here.
bool x11_stacked(const View &view) {
	return view.is_x11() && !view.override_redirect
		&& !wl_list_empty(&view.x11->stack_link);
}

} // namespace

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
	x11_set_geometry_.connect<&View::handle_x11_set_geometry>(x11->events.set_geometry, this);
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
	wlr_xwayland_surface_configure(x11, x11_x(), x11_y(), static_cast<uint16_t>(width),
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
	// A menu or a tooltip is placed by its own client, beside the window
	// it belongs to, and is neither decorated nor listed.
	override_redirect = x11->override_redirect;
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

// Where an override-redirect window asked to be: X11 places it itself, in
// the space the compositor configures its windows in, the canvas less the
// shared offset.
void View::x11_place_override_redirect() {
	x = x11->x + server.x11_offset_x;
	y = x11->y + server.x11_offset_y;
}

int16_t View::x11_x() const {
	return x11_coordinate(x - server.x11_offset_x);
}

int16_t View::x11_y() const {
	return x11_coordinate(y - server.x11_offset_y);
}

void View::x11_follow_canvas() {
	int16_t want_x = x11_x(), want_y = x11_y();
	if (x11->x == want_x && x11->y == want_y) {
		return;
	}
	wlr_xwayland_surface_configure(x11, want_x, want_y, x11->width, x11->height);
}

void View::handle_x11_configure(wlr_xwayland_surface_configure_event *event) {
	if (override_redirect) {
		wlr_xwayland_surface_configure(x11, event->x, event->y, event->width,
			event->height);
		damage();
		x11_place_override_redirect();
		damage();
		return;
	}
	// A window the compositor is not placing itself gets the size it asks
	// for, where the canvas has put it.
	if (maximized || fullscreen || tiled) {
		configure_size(shell_geometry().width, shell_geometry().height);
		return;
	}
	wlr_xwayland_surface_configure(x11, x11_x(), x11_y(), event->width, event->height);
	damage();
}

// An override-redirect window its client moved: the canvas follows. A
// managed window's geometry is the compositor's own, echoed back.
void View::handle_x11_set_geometry() {
	if (!override_redirect || !mapped) {
		return;
	}
	damage();
	x11_place_override_redirect();
	damage();
	server.visibility_dirty = true;
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

void Server::sync_x11() {
	if (xwayland == nullptr) {
		return;
	}
	// Bottom first: the minimized under everything, then the canvas's order.
	std::vector<View *> stack;
	for (View *view : views) {
		if (!view->is_x11() || view->override_redirect) {
			continue;
		}
		view->x11_follow_canvas();
		if (view->minimized && x11_stacked(*view)) {
			stack.push_back(view);
		}
	}
	for (View *view : std::views::reverse(views)) {
		if (!view->minimized && x11_stacked(*view)) {
			stack.push_back(view);
		}
	}
	if (std::ranges::equal(stack, x11_stack_, {}, &View::session_id)) {
		return;
	}
	x11_stack_.clear();
	for (View *view : stack) {
		// Each goes above the managed windows already restacked.
		wlr_xwayland_surface_restack(view->x11, nullptr, XCB_STACK_MODE_ABOVE);
		x11_stack_.push_back(view->session_id);
	}
}

void Server::x11_pointer_at(wlr_surface *surface, double sx, double sy) {
	View *view = surface != nullptr ? View::from_x11_surface(surface) : nullptr;
	Output *output = output_under_cursor();
	if (view == nullptr || output == nullptr) {
		return;
	}
	wlr_box layout{};
	wlr_output_layout_get_box(output_layout, nullptr, &layout);
	// X's screen runs from X's origin to the far corner of the layout.
	double screen_w = layout.x + layout.width;
	double screen_h = layout.y + layout.height;
	// Zoomed out, X's pointer moves faster than the cursor and could outrun
	// a move of the windows: it is brought back before it nears an edge.
	double zoom = output->camera.drawn().zoom;
	double margin = zoom < 1
		? std::min<double>(config::x11_pointer_margin, std::min(screen_w, screen_h) / 4)
		: 0;
	// Where Xwayland puts its pointer: the window's X position plus the event's.
	double rx = view->x11->x + sx;
	double ry = view->x11->y + sy;
	if (rx >= margin && rx < screen_w - margin && ry >= margin && ry < screen_h - margin) {
		return;
	}
	// The offset that puts X's pointer where the cursor is, or zoomed out in
	// the middle of X's screen; every X11 window moves with it.
	double to_x = zoom < 1 ? screen_w / 2 : cursor->x;
	double to_y = zoom < 1 ? screen_h / 2 : cursor->y;
	int dx = static_cast<int>(std::lround(view->x + sx - to_x));
	int dy = static_cast<int>(std::lround(view->y + sy - to_y));
	if (dx == x11_offset_x && dy == x11_offset_y) {
		return;
	}
	x11_offset_x = dx;
	x11_offset_y = dy;
	for (View *each : views) {
		if (each->is_x11()) {
			each->x11_follow_canvas();
		}
	}
}

#else // CANVAS_HAVE_XWAYLAND

// Built without Xwayland: the compositor runs, and a machine with no
// Xwayland simply has no X11 (docs/intents/windows/x11.md).
void Server::init_xwayland() {
	wlr_log(WLR_INFO, "built without Xwayland: this session has no X11");
}

void Server::finish_xwayland() {}
void Server::handle_xwayland_ready() {}
void Server::sync_x11() {}
void Server::x11_pointer_at(wlr_surface *, double, double) {}

View *View::from_x11_surface(wlr_surface *) {
	return nullptr;
}

#endif
