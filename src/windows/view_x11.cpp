// The X11 half of View: everything an X11 window answers differently from a
// wayland one. Its twin is view_xdg.cpp; the canvas logic they share is
// view.cpp, and the X server and X's picture of the windows are xwm.cpp's.
//
// An X11 window is a view like any other — title bar, desktop, overview,
// minimize, maximize, fullscreen, close (docs/intents/windows/x11.md).
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include "core/server.h"
#include "windows/view_x11.h"
#include "windows/xwm.h"

X11View::X11View(Server &server, wlr_xwayland_surface *wlr)
	: View(server), wlr(wlr) {
	wlr->data = static_cast<View *>(this);
	// An X11 window exists before it has a wayland surface: the pair is
	// made and broken by associate and dissociate, and only then are the
	// surface's own events there to listen to.
	x11_associate_.connect<&X11View::handle_x11_associate>(wlr->events.associate, this);
	x11_dissociate_.connect<&X11View::handle_x11_dissociate>(wlr->events.dissociate, this);
	destroy_.connect<&X11View::handle_destroy>(wlr->events.destroy, this);
	request_move_.connect<&X11View::handle_request_move>(wlr->events.request_move, this);
	request_resize_.connect<&X11View::handle_x11_request_resize>(
		wlr->events.request_resize, this);
	x11_configure_.connect<&X11View::handle_x11_configure>(
		wlr->events.request_configure, this);
	x11_set_geometry_.connect<&X11View::handle_x11_set_geometry>(
		wlr->events.set_geometry, this);
	request_maximize_.connect<&X11View::handle_x11_request_maximize>(
		wlr->events.request_maximize, this);
	request_minimize_.connect<&X11View::handle_x11_request_minimize>(
		wlr->events.request_minimize, this);
	request_fullscreen_.connect<&X11View::handle_x11_request_fullscreen>(
		wlr->events.request_fullscreen, this);
	set_title_.connect<&X11View::handle_set_names>(wlr->events.set_title, this);
	set_app_id_.connect<&X11View::handle_set_names>(wlr->events.set_class, this);
	set_parent_.connect<&X11View::handle_x11_set_parent>(wlr->events.set_parent, this);
	map_request_.connect<&X11View::handle_x11_map_request>(wlr->events.map_request, this);
}

X11View::~X11View() {
	drop_state_ask();
}

wlr_surface *X11View::surface() const {
	return wlr->surface;
}

wlr_box X11View::shell_geometry() const {
	// X11 has no window geometry of its own: the window is its surface.
	if (wlr->surface != nullptr && wlr->surface->mapped) {
		return wlr_box{0, 0, wlr->surface->current.width,
			wlr->surface->current.height};
	}
	return wlr_box{0, 0, wlr->width, wlr->height};
}

bool X11View::initialized() const {
	return wlr->surface != nullptr;
}

void X11View::send_close() {
	wlr_xwayland_surface_close(wlr);
}

void X11View::configure_size(int width, int height) {
	if (width <= 0 || height <= 0) {
		return; // X11 has no "your own size": the window keeps what it has
	}
	x11_configure(x11_x(), x11_y(), static_cast<uint16_t>(width), static_cast<uint16_t>(height));
}

void X11View::tell_maximized(bool want) {
	wlr_xwayland_surface_set_maximized(wlr, want, want);
}

void X11View::tell_fullscreen(bool want) {
	wlr_xwayland_surface_set_fullscreen(wlr, want);
}

void X11View::tell_tiled(bool) {
	// X11 has no tiled state to tell; the slice's size is the configure.
}

void X11View::tell_activated(bool want) {
	wlr_xwayland_surface_activate(wlr, want);
}

const char *X11View::shell_title() const {
	return wlr->title;
}

const char *X11View::shell_app_id() const {
	return wlr->class_;
}

pid_t X11View::client_pid() const {
	return static_cast<pid_t>(wlr->pid);
}

bool X11View::wants_maximized() const {
	return wlr->maximized_horz && wlr->maximized_vert;
}

bool X11View::wants_fullscreen() const {
	return wlr->fullscreen;
}

View *X11View::shell_parent() const {
	return wlr->parent != nullptr ? static_cast<View *>(wlr->parent->data) : nullptr;
}

// _NET_WM_STATE_MODAL: the window holds its parent's input until it goes.
bool X11View::modal() const {
	return wlr->modal;
}

bool X11View::client_placed() const {
	return override_redirect;
}

// There is no decoration protocol to negotiate with: the window manager
// decorates, which here means our own title bar (docs/intents/windows/x11.md).
bool X11View::decorated_without_negotiating() const {
	return !override_redirect;
}

// wlroots reads WM_NORMAL_HINTS and leaves a limit the client did not state
// at -1.
SizeLimit X11View::min_size() const {
	const xcb_size_hints_t *hints = wlr->size_hints;
	if (hints == nullptr) {
		return {};
	}
	return {std::max(hints->min_width, 0), std::max(hints->min_height, 0)};
}

SizeLimit X11View::max_size() const {
	const xcb_size_hints_t *hints = wlr->size_hints;
	if (hints == nullptr) {
		return {};
	}
	return {std::max(hints->max_width, 0), std::max(hints->max_height, 0)};
}

void X11View::for_each_surface(
		std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const {
	if (wlr->surface == nullptr) {
		return;
	}
	wlr_surface_for_each_surface(
		wlr->surface,
		[](wlr_surface *surface, int sx, int sy, void *data) {
			auto &callback =
				*static_cast<std::function_ref<void(wlr_surface *, int, int)> *>(data);
			callback(surface, sx, sy);
		},
		&fn);
}

wlr_surface *X11View::surface_at(double lx, double ly, double *sx, double *sy) const {
	if (wlr->surface == nullptr) {
		return nullptr;
	}
	return wlr_surface_surface_at(wlr->surface, lx, ly, sx, sy);
}

X11View *X11View::from_surface(wlr_surface *surface) {
	wlr_xwayland_surface *xwl = wlr_xwayland_surface_try_from_wlr_surface(
		wlr_surface_get_root_surface(surface));
	return xwl != nullptr ? static_cast<X11View *>(static_cast<View *>(xwl->data)) : nullptr;
}

void X11View::begin_interactive_resize(uint32_t) {
	// Nothing is held: the new place and size go to X in one configure.
}

void X11View::resize_to(wlr_box content) {
	move(content.x, content.y);
	x11_configure(x11_x(), x11_y(), static_cast<uint16_t>(content.width),
		static_cast<uint16_t>(content.height));
}

void X11View::end_interactive_resize() {
	// X acks nothing: the resize ends with the button.
}

void X11View::drop_resize() {}

void X11View::handle_x11_associate() {
	// A menu or a tooltip is placed by its own client, beside the window
	// it belongs to, and is neither decorated nor listed.
	override_redirect = wlr->override_redirect;
	// The window now has a surface: from here it behaves as any view does.
	map_.connect<&X11View::handle_x11_map>(wlr->surface->events.map, this);
	unmap_.connect<&X11View::handle_unmap>(wlr->surface->events.unmap, this);
	commit_.connect<&X11View::handle_commit>(wlr->surface->events.commit, this);
	handle_first_commit();
	// A surface Xwayland drew on before wlroots paired it with its window
	// maps only on its next commit, which a window that never redraws never
	// makes: map it now, as wlroots' own role does for a commit with a buffer.
	if (!wlr->surface->mapped && wlr_surface_has_buffer(wlr->surface)) {
		wlr_surface_map(wlr->surface);
	}
}

void X11View::handle_x11_dissociate() {
	map_.disconnect();
	unmap_.disconnect();
	commit_.disconnect();
}

void X11View::handle_x11_map() {
	if (override_redirect) {
		x11_place_override_redirect();
	}
	handle_map();
}

// Where an override-redirect window asked to be: X11 places it itself, in
// the space the compositor configures its windows in, the canvas less the
// shared offset.
void X11View::x11_place_override_redirect() {
	x = server.xwm.canvas_x(wlr->x);
	y = server.xwm.canvas_y(wlr->y);
}

int16_t X11View::x11_x() const {
	return server.xwm.x_position(x);
}

int16_t X11View::x11_y() const {
	return server.xwm.y_position(y);
}

bool X11View::x11_follow_canvas() {
	int16_t want_x = x11_x(), want_y = x11_y();
	if (wlr->x == want_x && wlr->y == want_y) {
		return false;
	}
	x11_configure(want_x, want_y, wlr->width, wlr->height);
	return true;
}

void X11View::x11_configure(int16_t x, int16_t y, uint16_t width, uint16_t height) {
	wlr_xwayland_surface_configure(wlr, x, y, width, height);
	server.xwm.changed();
}

void X11View::handle_x11_configure(wlr_xwayland_surface_configure_event *event) {
	if (override_redirect) {
		x11_configure(event->x, event->y, event->width, event->height);
		damage();
		x11_place_override_redirect();
		damage();
		return;
	}
	// A window the compositor is not placing itself gets the size it asks
	// for, where the canvas has put it.
	if (mode != WindowMode::normal || tiled) {
		configure_size(shell_geometry().width, shell_geometry().height);
		return;
	}
	x11_configure(x11_x(), x11_y(), event->width, event->height);
	damage();
}

// An override-redirect window its client moved: the canvas follows. A
// managed window's geometry is the compositor's own, echoed back.
void X11View::handle_x11_set_geometry() {
	if (!override_redirect || !mapped) {
		return;
	}
	damage();
	x11_place_override_redirect();
	visibility_changed();
}

void X11View::handle_x11_request_resize(wlr_xwayland_resize_event *event) {
	handle_request_resize(event->edges);
}

void X11View::handle_x11_request_maximize() {
	set_maximized(wlr->maximized_horz && wlr->maximized_vert);
}

void X11View::handle_x11_request_minimize() {
	set_minimized(wlr->minimized);
}

void X11View::handle_x11_request_fullscreen() {
	set_fullscreen(wlr->fullscreen);
}

// The parent decides the kind, and the client may set it after the window
// maps (or have it dropped with the parent's death): the window is laid out
// again by the rules of the map. wlroots reports no change of the modal
// state, so it is read when the parent arrives; the window type decides no
// kind and needs no listener.
void X11View::handle_x11_set_parent() {
	handle_kind_change();
}

// wlroots rewrites _NET_WM_STATE when the window maps, from what it has read
// of it, and reads it only at the association, which comes after that
// rewrite. A client that sets the state and maps in one flush would lose
// _NET_WM_STATE_MODAL before wlroots saw it. The map request comes first,
// with the property as the client left it: the compositor asks X for it
// there, and the answer waits for the map notify.
void X11View::handle_x11_map_request() {
	drop_state_ask();
	state_ask_ = server.xwm.ask_state(wlr->window_id);
}

// X sent the answer before this MapNotify, on the same connection, so the
// connection already holds it: taking it waits for nothing, however slow X
// is. Setting the modal state here, ahead of wlroots' handling of the same
// event, makes its rewrite keep it.
void X11View::handle_x11_map_notify() {
	if (!state_ask_.has_value()) {
		return;
	}
	unsigned int ask = *std::exchange(state_ask_, std::nullopt);
	bool modal = server.xwm.state_modal(ask);
	wlr->modal = wlr->modal || modal;
}

void X11View::drop_state_ask() {
	if (state_ask_.has_value()) {
		server.xwm.drop_ask(*std::exchange(state_ask_, std::nullopt));
	}
}

