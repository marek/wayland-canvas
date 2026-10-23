// The X11 half of View, and the Xwayland server behind it: everything an
// X11 window answers differently from a wayland one. Its twin is
// view_xdg.cpp; the canvas logic they share is view.cpp.
//
// An X11 window is a view like any other — title bar, desktop, overview,
// minimize, maximize, fullscreen, close (docs/intents/windows/x11.md).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <xcb/xcbext.h>
#include "core/config.h"
#include "core/server.h"
#include "platform/output.h"
#include "windows/view_x11.h"

namespace {

// An event's type is its response type less the top bit, which marks one
// sent by another client.
constexpr uint8_t x11_event_type_mask = 0x7f;

// X positions are 16-bit: a window farther than that from the pointer waits
// at the limit (docs/intents/windows/x11.md).
int16_t x11_coordinate(int value) {
	return static_cast<int16_t>(std::clamp<int>(value, INT16_MIN, INT16_MAX));
}

// Whether X stacks the window: wlroots puts it in its stack on X's
// MapNotify, at the bottom, which can come after the surface maps here.
bool x11_stacked(const X11View &view) {
	return !view.override_redirect && !wl_list_empty(&view.wlr->stack_link);
}

// Xwayland's window manager connection; nullptr while Xwayland is not up.
xcb_connection_t *window_manager_connection(const Server &server) {
	return server.xwayland != nullptr ? wlr_xwayland_get_xwm_connection(server.xwayland)
		: nullptr;
}

} // namespace

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

// ---- what the shared code asks ----

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

// ---- an interactive resize ----

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

// ---- the X11 window's own events ----

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
	x = wlr->x + server.x11_offset_x;
	y = wlr->y + server.x11_offset_y;
}

int16_t X11View::x11_x() const {
	return x11_coordinate(x - server.x11_offset_x);
}

int16_t X11View::x11_y() const {
	return x11_coordinate(y - server.x11_offset_y);
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
	server.x11_changed();
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
	state_ask_ = server.x11_ask_state(wlr->window_id);
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
	bool modal = server.x11_state_modal(ask);
	wlr->modal = wlr->modal || modal;
}

void X11View::drop_state_ask() {
	if (state_ask_.has_value()) {
		server.x11_drop_ask(*std::exchange(state_ask_, std::nullopt));
	}
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
	xwayland->data = this;
	xwayland->user_event_handler = [](wlr_xwayland *wlr, xcb_generic_event_t *event) {
		static_cast<Server *>(wlr->data)->handle_x11_event(*event);
		return false;
	};
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
	// A new window manager connection: an ask on the one before is void.
	x11_settle_ask_.reset();
	x11_unasked_ = false;
	// The atoms of the state hint wlroots rewrites before it reads it
	// (x11_ask_state); each X server numbers its own.
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(xwayland);
	static constexpr std::string_view state_name = "_NET_WM_STATE",
		modal_name = "_NET_WM_STATE_MODAL";
	xcb_intern_atom_cookie_t state = xcb_intern_atom(conn, 0,
		static_cast<uint16_t>(state_name.size()), state_name.data());
	xcb_intern_atom_cookie_t modal = xcb_intern_atom(conn, 0,
		static_cast<uint16_t>(modal_name.size()), modal_name.data());
	auto atom_of = [conn](xcb_intern_atom_cookie_t cookie) {
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, nullptr);
		xcb_atom_t atom = reply != nullptr ? reply->atom : XCB_ATOM_NONE;
		std::free(reply);
		return atom;
	};
	x11_atom_state_ = atom_of(state);
	x11_atom_modal_ = atom_of(modal);
}

void Server::handle_new_xwayland_surface(wlr_xwayland_surface *surface) {
	toplevels.push_back(std::make_unique<X11View>(*this, surface));
}

// The map notify is where the answer to the state ask is in
// (X11View::handle_x11_map_notify).
void Server::handle_x11_event(xcb_generic_event_t &event) {
	if ((event.response_type & x11_event_type_mask) != XCB_MAP_NOTIFY) {
		return;
	}
	xcb_window_t window = reinterpret_cast<xcb_map_notify_event_t *>(&event)->window;
	for (const std::unique_ptr<View> &view : toplevels) {
		X11View *x11 = view->as_x11();
		if (x11 != nullptr && x11->wlr->window_id == window) {
			x11->handle_x11_map_notify();
			return;
		}
	}
}

// Not flushed: wlroots flushes after the event it is handling, and the
// MapWindow it sends next goes out behind this request.
std::optional<unsigned int> Server::x11_ask_state(xcb_window_t window) {
	xcb_connection_t *conn = window_manager_connection(*this);
	if (conn == nullptr || x11_atom_state_ == XCB_ATOM_NONE || x11_atom_modal_ == XCB_ATOM_NONE) {
		return std::nullopt;
	}
	return xcb_get_property(conn, 0, window, x11_atom_state_, XCB_ATOM_ATOM, 0, 32).sequence;
}

bool Server::x11_state_modal(unsigned int ask) {
	xcb_connection_t *conn = window_manager_connection(*this);
	if (conn == nullptr) {
		return false;
	}
	xcb_generic_error_t *error = nullptr;
	xcb_get_property_reply_t *reply = xcb_get_property_reply(conn,
		xcb_get_property_cookie_t{ask}, &error);
	std::free(error);
	if (reply == nullptr) {
		return false;
	}
	std::span<const xcb_atom_t> states(
		static_cast<const xcb_atom_t *>(xcb_get_property_value(reply)),
		static_cast<size_t>(xcb_get_property_value_length(reply)) / sizeof(xcb_atom_t));
	bool modal = std::ranges::contains(states, x11_atom_modal_);
	std::free(reply);
	return modal;
}

void Server::x11_drop_ask(unsigned int ask) {
	if (xcb_connection_t *conn = window_manager_connection(*this); conn != nullptr) {
		xcb_discard_reply(conn, ask);
	}
}

void Server::sync_x11() {
	if (xwayland == nullptr) {
		return;
	}
	// Bottom first: the minimized under everything, then the canvas's order.
	std::vector<X11View *> stack;
	bool moved = false;
	for (View *view : views) {
		X11View *x11 = view->as_x11();
		if (x11 == nullptr || x11->override_redirect) {
			continue;
		}
		moved = x11->x11_follow_canvas() || moved;
		if (x11->minimized && x11_stacked(*x11)) {
			stack.push_back(x11);
		}
	}
	if (moved) {
		x11_put_pointer();
	}
	for (View *view : std::views::reverse(views)) {
		X11View *x11 = view->as_x11();
		if (x11 != nullptr && !x11->minimized && x11_stacked(*x11)) {
			stack.push_back(x11);
		}
	}
	if (!std::ranges::equal(stack, x11_stack_, {}, &View::session_id)) {
		x11_stack_.clear();
		for (X11View *x11 : stack) {
			// Each goes above the managed windows already restacked.
			wlr_xwayland_surface_restack(x11->wlr, nullptr, XCB_STACK_MODE_ABOVE);
			x11_stack_.push_back(x11->session_id);
		}
		x11_changed();
	}
	// Asked now, X's answer is usually in before the next press needs it.
	if (x11_unasked_) {
		x11_ask_settled();
	}
}

void Server::x11_follow_pointer() {
	Output *output = output_at(cursor->x, cursor->y);
	if (xwayland == nullptr || output == nullptr || !output->interactive()
			|| std::ranges::none_of(views, [](const View *view) {
				return view->as_x11() != nullptr;
			})) {
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
	// Where Xwayland puts its pointer over an X11 window here: the canvas
	// point under the cursor less the offset, wherever the pointer is, so
	// entering a window meets one X already has in place.
	Point p = output->local_to_canvas(cursor->x - output->lx, cursor->y - output->ly);
	double rx = p.x - x11_offset_x;
	double ry = p.y - x11_offset_y;
	if (rx >= margin && rx < screen_w - margin && ry >= margin && ry < screen_h - margin) {
		return;
	}
	// The offset that puts X's pointer where the cursor is, or zoomed out in
	// the middle of X's screen; every X11 window moves with it.
	double to_x = zoom < 1 ? screen_w / 2 : cursor->x;
	double to_y = zoom < 1 ? screen_h / 2 : cursor->y;
	int dx = static_cast<int>(std::lround(p.x - to_x));
	int dy = static_cast<int>(std::lround(p.y - to_y));
	if (dx == x11_offset_x && dy == x11_offset_y) {
		return;
	}
	x11_offset_x = dx;
	x11_offset_y = dy;
	for (View *each : views) {
		if (X11View *x11 = each->as_x11()) {
			x11->x11_follow_canvas();
		}
	}
	x11_put_pointer();
	x11_ask_settled();
}

// X hears of its windows' moves on the window manager's connection and of
// the pointer on Xwayland's wayland one, and may take the pointer event
// first and keep it while the windows move away under it. X takes one
// connection's requests in order, so a warp sent on the window manager's
// own, after the moves, leaves X's pointer where the cursor is whichever it
// took first. Over anything but an X11 window, X's pointer is in no window.
void Server::x11_put_pointer() {
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(xwayland);
	Output *output = output_at(cursor->x, cursor->y);
	if (conn == nullptr || output == nullptr) {
		return;
	}
	Point p = output->local_to_canvas(cursor->x - output->lx, cursor->y - output->ly);
	std::optional<ViewHit> hit = view_at(p.x, p.y, output->camera.drawn().zoom);
	if (!hit.has_value() || hit->view->as_x11() == nullptr) {
		return;
	}
	const X11View &x11 = *hit->view->as_x11();
	// Where Xwayland puts its pointer for the same event: the window's X
	// position plus the whole pixels of the point on its surface.
	int16_t x = x11_coordinate(
		x11.x11_x() + wl_fixed_to_int(wl_fixed_from_double(hit->sx)));
	int16_t y = x11_coordinate(
		x11.x11_y() + wl_fixed_to_int(wl_fixed_from_double(hit->sy)));
	xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root;
	xcb_warp_pointer(conn, XCB_NONE, root, 0, 0, 0, 0, x, y);
	xcb_flush(conn);
}

void Server::x11_changed() {
	x11_unasked_ = true;
}

// GetInputFocus answers at once: X sending it means X has taken in every
// request sent before it on this connection, the moves and the warp too.
void Server::x11_ask_settled() {
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(xwayland);
	if (conn == nullptr) {
		return;
	}
	if (x11_settle_ask_.has_value()) {
		xcb_discard_reply(conn, *x11_settle_ask_); // the new ask answers for both
	}
	x11_settle_ask_ = xcb_get_input_focus(conn).sequence;
	x11_unasked_ = false;
	xcb_flush(conn);
}

// Reads what X has sent without waiting; wlroots handles any event read here
// at the end of the same dispatch, as it checks its connection every time.
bool Server::x11_settled() {
	xcb_connection_t *conn = xwayland != nullptr ? wlr_xwayland_get_xwm_connection(xwayland)
		: nullptr;
	if (conn == nullptr) {
		x11_settle_ask_.reset();
		x11_unasked_ = false;
		return true;
	}
	if (x11_unasked_) {
		x11_ask_settled();
	}
	if (!x11_settle_ask_.has_value()) {
		return true;
	}
	void *reply = nullptr;
	xcb_generic_error_t *error = nullptr;
	if (xcb_poll_for_reply(conn, *x11_settle_ask_, &reply, &error) == 0) {
		return false;
	}
	std::free(reply);
	std::free(error);
	x11_settle_ask_.reset();
	return true;
}

void Server::x11_give_up() {
	xcb_connection_t *conn = xwayland != nullptr ? wlr_xwayland_get_xwm_connection(xwayland)
		: nullptr;
	if (conn != nullptr && x11_settle_ask_.has_value()) {
		xcb_discard_reply(conn, *x11_settle_ask_);
	}
	x11_settle_ask_.reset();
}

bool Server::on_x11_window(wlr_surface *surface) {
	return surface != nullptr && X11View::from_surface(surface) != nullptr;
}
