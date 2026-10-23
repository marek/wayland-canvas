#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include "canvas/desktop.h"
#include "windows/decoration.h"
#include "windows/foreign_toplevel.h"
#include "windows/launches.h"
#include "windows/view_kind.h"
#include "core/geometry.h"
#include "core/util.h"

class Output;
class Server;
class X11View;

// A size a window limits itself to; 0 in a dimension is no limit.
struct SizeLimit {
	int width = 0, height = 0;
};

// What a window is showing as; one at a time. `normal` is the window's own
// box, any other fills an area the compositor picks (windows/placement.cpp).
enum class WindowMode {
	normal,
	maximized, // the output's usable area, the title bar kept
	fullscreen, // the whole output, no decorations
};

// A window's place on a tiled desktop's board: the desktop, and its slice of
// the board in board fractions.
struct BoardSeat {
	DesktopId desktop;
	Slice slice;
};

// A window on the canvas, whichever shell its client speaks. The canvas, the
// renderer, the input path and the capture path know only this class; the
// two shells are XdgView (view_xdg.h) and X11View (view_x11.h), each holding
// its own wlroots object and answering the pure virtuals below with it.
class View {
public:
	virtual ~View();
	View(const View &) = delete;
	View &operator=(const View &) = delete;

	// The view whose toplevel surface tree holds `surface`, or nullptr.
	static View *from_surface(wlr_surface *surface);

	wlr_box content_box() const; // window geometry on the canvas
	wlr_box frame_box() const; // content plus title bar and border
	Rect frame_rect() const;
	// Canvas position of the surface tree's origin: the window geometry's
	// corner sits at (x, y), and a window with no geometry yet has its base
	// surface fill it from there.
	Point surface_origin() const;
	void close();
	void set_minimized(bool minimized);
	void set_maximized(bool maximized); // false leaves only maximized
	void set_fullscreen(bool fullscreen); // false leaves only fullscreen
	void set_mode(WindowMode mode);
	void set_tiled(bool tiled); // the xdg tiled state only, no move or resize
	// Slice placement: sized to the slice's box and told it is tiled.
	void set_frame(wlr_box frame);
	void move(int x, int y);
	// Moves the window and the box it restores to by the same offset.
	void translate(int dx, int dy);
	void activate();
	void damage();
	// What the view shows, or where, changed: repaints it and has every
	// window's outputs worked out again before the next frame.
	void visibility_changed();
	// The outputs the view is visible on are now `mask`: its surfaces are told
	// they entered or left and at what scale, and the foreign toplevel handle
	// follows.
	void update_outputs(uint32_t mask);
	// The ext-foreign-toplevel-list handle's identifier, or null before the
	// window maps (or without the capture protocols). Window capture clients
	// (grim -T, the portal's window sharing) name windows by it.
	const char *ext_toplevel_identifier() const;

	// What the shared code asks of the window; each shell answers with its
	// own protocol, so view.cpp holds no shell-specific line.
	virtual wlr_surface *surface() const = 0;
	virtual wlr_box shell_geometry() const = 0;
	virtual bool initialized() const = 0;
	virtual void send_close() = 0;
	virtual void configure_size(int width, int height) = 0;
	virtual void tell_maximized(bool want) = 0;
	virtual void tell_fullscreen(bool want) = 0;
	virtual void tell_tiled(bool want) = 0;
	virtual void tell_activated(bool want) = 0;
	virtual const char *shell_title() const = 0;
	virtual const char *shell_app_id() const = 0;
	virtual pid_t client_pid() const = 0;
	// What the window asked to open as.
	virtual bool wants_maximized() const = 0;
	virtual bool wants_fullscreen() const = 0;
	// The window it belongs to: an xdg toplevel's parent, an X11 window's
	// WM_TRANSIENT_FOR; nullptr for none.
	virtual View *shell_parent() const = 0;
	// Whether the window holds its parent's input until it goes: X11's
	// _NET_WM_STATE_MODAL, xdg-dialog-v1's modal.
	virtual bool modal() const = 0;
	// Whether the client places the window itself and the window manager must
	// not decorate, list or place it: an X11 override-redirect window, which
	// is a menu, a tooltip or a drag icon (docs/intents/windows/x11.md).
	virtual bool client_placed() const = 0;
	// Whether the shell has no decoration protocol to negotiate, so the
	// window manager decorates the window and its title bar is ours.
	virtual bool decorated_without_negotiating() const = 0;
	// The sizes the client limits itself to.
	virtual SizeLimit min_size() const = 0;
	virtual SizeLimit max_size() const = 0;
	// Every mapped surface of the view, with its position relative to the window geometry.
	virtual void for_each_surface(
		std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const = 0;
	// The surface at a point in the view's own coordinates, with the
	// point in that surface's coordinates; nullptr for a miss.
	virtual wlr_surface *surface_at(double lx, double ly, double *sx, double *sy) const = 0;
	// An interactive resize by `edges`, driven by the pointer grab: begun, then
	// a content box on the canvas per motion, then ended. The edges not
	// dragged stay put.
	virtual void begin_interactive_resize(uint32_t edges) = 0;
	virtual void resize_to(wlr_box content) = 0;
	virtual void end_interactive_resize() = 0;
	// The X11 shell's own view of the window; nullptr for any other shell. Only the
	// X window manager asks.
	virtual X11View *as_x11() { return nullptr; }
	virtual const X11View *as_x11() const { return nullptr; }

	// What the window is to the window manager; every layout rule reads it
	// (windows/view_kind.h).
	ViewKind kind() const {
		if (client_placed()) {
			return ViewKind::client_placed;
		}
		if (modal()) {
			return ViewKind::modal;
		}
		return shell_parent() != nullptr ? ViewKind::dialog : ViewKind::normal;
	}

	Server &server;

	// Canvas position of the window geometry's top-left corner.
	int x = 0, y = 0;
	bool mapped = false, minimized = false, tiled = false;
	WindowMode mode = WindowMode::normal;
	// The window's seat on a tiled desktop's board. A zoom moves a frame's
	// edges past windows without moving them, so the seat, not the centre,
	// says whose it is until it leaves the board; a minimized window keeps
	// its seat remembered while it is away, and a maximized or fullscreen
	// one holds its slice reserved.
	std::optional<BoardSeat> board_seat;
	bool seated_on(DesktopId desktop) const {
		return board_seat.has_value() && board_seat->desktop == desktop;
	}
	wlr_box saved{}; // content box to restore when the window leaves maximized or fullscreen
	// Leaving maximize/fullscreen restores the size through a configure: the
	// board return waits for the commit that carries it.
	bool settle_pending = false;
	uint32_t output_mask = 0; // outputs the view is visible on
	double told_scale = 0; // the scale its surfaces were last told: their outputs'
	uint64_t minimize_seq = 0;
	// The spot of the launch from the overview the window's process descends
	// from: the window opens there. Traced at each initial commit while the
	// launch is recent (config::launch_trace_ms), dropped once it has mapped.
	std::optional<LaunchSpot> launched_for;
	// Stable id for the IPC world (capture picks and sharing), assigned in
	// the constructor.
	uint64_t session_id = 0;

	Decoration deco;

protected:
	explicit View(Server &server);

	// A resize hold ends because the window took another box (maximize,
	// fullscreen, a slice) or went away: nothing may still anchor it.
	virtual void drop_resize() = 0;

	// The shared half of the life cycle (view.cpp); each shell's own
	// listeners call into these.
	void handle_map();
	void handle_unmap();
	void handle_first_commit(); // the client's very first commit
	// What decides kind() can arrive after the map (an X11 client sets its
	// parent and modal state when it likes): the window joins or leaves a
	// board, and goes over its parent when it has become a dialog, as at the
	// map.
	void handle_kind_change();
	void handle_commit();
	void handle_destroy();
	void handle_request_move();
	void handle_request_resize(uint32_t edges);
	void handle_set_names(); // the title or the app id changed

private:
	// Changes the mode and tells the client and the foreign toplevel handle:
	// the old mode off, the new one on.
	void switch_mode(WindowMode want);
	void announce_mode(WindowMode of, bool on);

	// Created on map, closed on unmap.
	ForeignToplevel foreign_;
	// The kind the window was last laid out as: the map's, then each change.
	ViewKind laid_out_kind_ = ViewKind::normal;

	friend class Server;
};

// What a canvas point lies on: a surface of a view (surface-local coordinates), or a part
// of its title bar and border.
struct ViewHit {
	View *view = nullptr;
	wlr_surface *surface = nullptr; // nullptr on decorations
	double sx = 0, sy = 0;
	DecoPart part = DecoPart::none;
	uint32_t edges = 0;
};
