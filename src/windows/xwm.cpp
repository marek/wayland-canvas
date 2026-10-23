#include "windows/xwm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ranges>
#include <span>
#include <string_view>
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

} // namespace

Xwm::Xwm(Server &server) : server_(server) {}

void Xwm::init() {
	// Lazy: Xwayland starts with the first X11 client, not at login.
	wlr_ = wlr_xwayland_create(server_.display, server_.compositor, true);
	if (wlr_ == nullptr) {
		wlr_log(WLR_INFO, "Xwayland is not available: this session has no X11");
		return;
	}
	new_surface_.connect<&Xwm::handle_new_surface>(wlr_->events.new_surface, this);
	ready_.connect<&Xwm::handle_ready>(wlr_->events.ready, this);
	wlr_xwayland_set_seat(wlr_, server_.seat);
	setenv("DISPLAY", wlr_->display_name, true);
	wlr_->data = this;
	wlr_->user_event_handler = [](wlr_xwayland *wlr, xcb_generic_event_t *event) {
		static_cast<Xwm *>(wlr->data)->handle_event(*event);
		return false;
	};
}

void Xwm::finish() {
	new_surface_.disconnect();
	ready_.disconnect();
	if (wlr_ != nullptr) {
		wlr_xwayland_destroy(wlr_);
		wlr_ = nullptr;
	}
	unsetenv("DISPLAY");
}

int16_t Xwm::x_position(int canvas_x) const {
	return x11_coordinate(canvas_x - offset_x_);
}

int16_t Xwm::y_position(int canvas_y) const {
	return x11_coordinate(canvas_y - offset_y_);
}

int Xwm::canvas_x(int x_position) const {
	return x_position + offset_x_;
}

int Xwm::canvas_y(int y_position) const {
	return y_position + offset_y_;
}

xcb_connection_t *Xwm::connection() const {
	return wlr_ != nullptr ? wlr_xwayland_get_xwm_connection(wlr_) : nullptr;
}

void Xwm::handle_new_surface(wlr_xwayland_surface *surface) {
	server_.toplevels.push_back(std::make_unique<X11View>(server_, surface));
}

void Xwm::handle_ready() {
	// The clipboard goes both ways from here on: wlroots bridges the X11
	// selection to the seat's.
	wlr_xwayland_set_seat(wlr_, server_.seat);
	// A new window manager connection: an ask on the one before is void.
	settle_ask_.reset();
	unasked_ = false;
	// The atoms of the state hint wlroots rewrites before it reads it
	// (ask_state); each X server numbers its own.
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(wlr_);
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
	atom_state_ = atom_of(state);
	atom_modal_ = atom_of(modal);
}

// The map notify is where the answer to the state ask is in
// (X11View::handle_x11_map_notify).
void Xwm::handle_event(xcb_generic_event_t &event) {
	if ((event.response_type & x11_event_type_mask) != XCB_MAP_NOTIFY) {
		return;
	}
	xcb_window_t window = reinterpret_cast<xcb_map_notify_event_t *>(&event)->window;
	for (const std::unique_ptr<View> &view : server_.toplevels) {
		X11View *x11 = view->as_x11();
		if (x11 != nullptr && x11->wlr->window_id == window) {
			x11->handle_x11_map_notify();
			return;
		}
	}
}

// Not flushed: wlroots flushes after the event it is handling, and the
// MapWindow it sends next goes out behind this request.
std::optional<unsigned int> Xwm::ask_state(xcb_window_t window) {
	xcb_connection_t *conn = connection();
	if (conn == nullptr || atom_state_ == XCB_ATOM_NONE || atom_modal_ == XCB_ATOM_NONE) {
		return std::nullopt;
	}
	return xcb_get_property(conn, 0, window, atom_state_, XCB_ATOM_ATOM, 0, 32).sequence;
}

bool Xwm::state_modal(unsigned int ask) {
	xcb_connection_t *conn = connection();
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
	bool modal = std::ranges::contains(states, atom_modal_);
	std::free(reply);
	return modal;
}

void Xwm::drop_ask(unsigned int ask) {
	if (xcb_connection_t *conn = connection(); conn != nullptr) {
		xcb_discard_reply(conn, ask);
	}
}

void Xwm::sync() {
	if (wlr_ == nullptr) {
		return;
	}
	// Bottom first: the minimized under everything, then the canvas's order.
	std::vector<X11View *> stack;
	bool moved = false;
	for (View *view : server_.views) {
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
		put_pointer();
	}
	for (View *view : std::views::reverse(server_.views)) {
		X11View *x11 = view->as_x11();
		if (x11 != nullptr && !x11->minimized && x11_stacked(*x11)) {
			stack.push_back(x11);
		}
	}
	if (!std::ranges::equal(stack, stack_, {}, &View::session_id)) {
		stack_.clear();
		for (X11View *x11 : stack) {
			// Each goes above the managed windows already restacked.
			wlr_xwayland_surface_restack(x11->wlr, nullptr, XCB_STACK_MODE_ABOVE);
			stack_.push_back(x11->session_id);
		}
		changed();
	}
	// Asked now, X's answer is usually in before the next press needs it.
	if (unasked_) {
		ask_settled();
	}
}

void Xwm::follow_pointer() {
	Output *output = server_.output_at(server_.cursor->x, server_.cursor->y);
	if (wlr_ == nullptr || output == nullptr || !output->interactive()
			|| std::ranges::none_of(server_.views, [](const View *view) {
				return view->as_x11() != nullptr;
			})) {
		return;
	}
	wlr_box layout{};
	wlr_output_layout_get_box(server_.output_layout, nullptr, &layout);
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
	Point p = output->local_to_canvas(server_.cursor->x - output->lx,
		server_.cursor->y - output->ly);
	double rx = p.x - offset_x_;
	double ry = p.y - offset_y_;
	if (rx >= margin && rx < screen_w - margin && ry >= margin && ry < screen_h - margin) {
		return;
	}
	// The offset that puts X's pointer where the cursor is, or zoomed out in
	// the middle of X's screen; every X11 window moves with it.
	double to_x = zoom < 1 ? screen_w / 2 : server_.cursor->x;
	double to_y = zoom < 1 ? screen_h / 2 : server_.cursor->y;
	int dx = static_cast<int>(std::lround(p.x - to_x));
	int dy = static_cast<int>(std::lround(p.y - to_y));
	if (dx == offset_x_ && dy == offset_y_) {
		return;
	}
	offset_x_ = dx;
	offset_y_ = dy;
	for (View *each : server_.views) {
		if (X11View *x11 = each->as_x11()) {
			x11->x11_follow_canvas();
		}
	}
	put_pointer();
	ask_settled();
}

// X hears of its windows' moves on the window manager's connection and of
// the pointer on Xwayland's wayland one, and may take the pointer event
// first and keep it while the windows move away under it. X takes one
// connection's requests in order, so a warp sent on the window manager's
// own, after the moves, leaves X's pointer where the cursor is whichever it
// took first. Over anything but an X11 window, X's pointer is in no window.
void Xwm::put_pointer() {
	xcb_connection_t *conn = connection();
	Output *output = server_.output_at(server_.cursor->x, server_.cursor->y);
	if (conn == nullptr || output == nullptr) {
		return;
	}
	Point p = output->local_to_canvas(server_.cursor->x - output->lx,
		server_.cursor->y - output->ly);
	std::optional<ViewHit> hit = server_.view_at(p.x, p.y, output->camera.drawn().zoom);
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

void Xwm::changed() {
	unasked_ = true;
}

// GetInputFocus answers at once: X sending it means X has taken in every
// request sent before it on this connection, the moves and the warp too.
void Xwm::ask_settled() {
	xcb_connection_t *conn = connection();
	if (conn == nullptr) {
		return;
	}
	if (settle_ask_.has_value()) {
		xcb_discard_reply(conn, *settle_ask_); // the new ask answers for both
	}
	settle_ask_ = xcb_get_input_focus(conn).sequence;
	unasked_ = false;
	xcb_flush(conn);
}

// Reads what X has sent without waiting; wlroots handles any event read here
// at the end of the same dispatch, as it checks its connection every time.
bool Xwm::settled() {
	xcb_connection_t *conn = connection();
	if (conn == nullptr) {
		settle_ask_.reset();
		unasked_ = false;
		return true;
	}
	if (unasked_) {
		ask_settled();
	}
	if (!settle_ask_.has_value()) {
		return true;
	}
	void *reply = nullptr;
	xcb_generic_error_t *error = nullptr;
	if (xcb_poll_for_reply(conn, *settle_ask_, &reply, &error) == 0) {
		return false;
	}
	std::free(reply);
	std::free(error);
	settle_ask_.reset();
	return true;
}

void Xwm::give_up() {
	xcb_connection_t *conn = connection();
	if (conn != nullptr && settle_ask_.has_value()) {
		xcb_discard_reply(conn, *settle_ask_);
	}
	settle_ask_.reset();
}

bool Xwm::is_x11_window(wlr_surface *surface) {
	return surface != nullptr && X11View::from_surface(surface) != nullptr;
}
