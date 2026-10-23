#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include "core/util.h"

class Server;

// The X window manager (docs/intents/windows/x11.md): Xwayland, created at
// start and run by the first X11 client, the X11 windows it brings, and X's
// own picture of those windows, which follows the canvas. X positions are
// canvas positions less one offset shared by every X11 window; input for an
// X11 window waits until X has taken in what was sent to it.
class Xwm {
public:
	explicit Xwm(Server &server);
	Xwm(const Xwm &) = delete;
	Xwm &operator=(const Xwm &) = delete;

	// Creates the X server, started by its first client, and exports DISPLAY
	// for the clients the compositor starts; a machine without Xwayland logs
	// that and goes on. Needs the Wayland display, the compositor and the seat.
	void init();
	// Destroys the X server with the windows it brought and unsets DISPLAY.
	void finish();

	// nullptr when there is no X server.
	wlr_xwayland *wlr() const { return wlr_; }

	// Where X has a canvas coordinate, and the canvas coordinate of an X
	// position, through the shared offset.
	int16_t x_position(int canvas_x) const;
	int16_t y_position(int canvas_y) const;
	int canvas_x(int x_position) const;
	int canvas_y(int y_position) const;

	// Once per frame X is told the canvas's stacking and the windows' places.
	void sync();
	// At every pointer move: the offset moves if X's pointer would leave X's
	// screen.
	void follow_pointer();
	// A move, a resize or a restack went to X: input for an X11 window waits
	// until X has taken it in.
	void changed();
	// Whether X has taken in every change sent to it; asks X to say so when
	// changes went out since the last ask.
	bool settled();
	// X did not answer within config::x11_input_wait_ms: input goes on
	// without the answer, and the ask is dropped.
	void give_up();

	// A window's _NET_WM_STATE, as the client set it, asked of X at the map
	// request without waiting: the ask's sequence, nullopt when X is not
	// there. X answers before it sends the window's MapNotify, so the answer
	// is taken there (X11View::handle_x11_map_notify) and costs no wait.
	std::optional<unsigned int> ask_state(xcb_window_t window);
	// Whether the answer to `ask` holds _NET_WM_STATE_MODAL; takes the answer.
	bool state_modal(unsigned int ask);
	// Gives up an answer that will not be taken.
	void drop_ask(unsigned int ask);

	// Whether `surface` belongs to an X11 window: input to one waits for X.
	static bool is_x11_window(wlr_surface *surface);

private:
	void handle_new_surface(wlr_xwayland_surface *surface);
	void handle_ready();
	// Sees every X event before wlroots handles it; wlroots still handles it.
	void handle_event(xcb_generic_event_t &event);
	// The window manager's connection to X; nullptr while there is none.
	xcb_connection_t *connection() const;
	// After X's windows move, X's pointer is put where the cursor is over
	// them, on the window manager's own connection, so X applies it after
	// the moves.
	void put_pointer();
	// Asks X, on the window manager's connection, to answer once it has
	// taken in everything sent before.
	void ask_settled();

	Server &server_;
	wlr_xwayland *wlr_ = nullptr;
	Listener new_surface_, ready_;
	int offset_x_ = 0, offset_y_ = 0;
	// X's stacking as last sent, bottom first, by view session id.
	std::vector<uint64_t> stack_;
	// The settle ask's sequence, and whether changes went out since it was sent.
	std::optional<unsigned int> settle_ask_;
	bool unasked_ = false;
	// The atoms of _NET_WM_STATE and _NET_WM_STATE_MODAL on the running X server.
	uint32_t atom_state_ = 0, atom_modal_ = 0;
};
