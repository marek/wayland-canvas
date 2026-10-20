#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include "canvas/desktop.h"
#include "windows/decoration.h"
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
	void close();
	void set_minimized(bool minimized);
	void set_maximized(bool maximized);
	void set_fullscreen(bool fullscreen);
	void set_tiled(bool tiled); // the xdg tiled state only, no move or resize
	// Slice placement: sized to the slice's box and told it is tiled.
	void set_frame(wlr_box frame);
	void move(int x, int y);
	void activate();
	void damage();
	// Foreign toplevel handle: the view entered or left `output`'s viewport.
	void foreign_output_enter(Output &output);
	void foreign_output_leave(Output &output);
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
	bool mapped = false, minimized = false, maximized = false, fullscreen = false,
		tiled = false;
	// The tiled desktop whose board holds the window. A zoom moves a frame's
	// edges past windows without moving them, so the board, not the centre,
	// says whose it is until it leaves the board; a minimized window keeps
	// its slice remembered while it is away, and a maximized or fullscreen
	// one holds its slice reserved.
	std::optional<DesktopId> board;
	// The window's slice of that desktop's board, board fractions.
	std::optional<Slice> slice;
	wlr_box saved{}; // content box to restore after maximize/fullscreen
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
	void handle_commit();
	void handle_destroy();
	void handle_request_move();
	void handle_request_resize(uint32_t edges);
	void handle_set_title();
	void handle_set_app_id();

private:
	void create_foreign();
	void destroy_foreign();

	void handle_foreign_activate();
	void handle_foreign_maximize(const wlr_foreign_toplevel_handle_v1_maximized_event *event);
	void handle_foreign_minimize(const wlr_foreign_toplevel_handle_v1_minimized_event *event);
	void handle_foreign_fullscreen(const wlr_foreign_toplevel_handle_v1_fullscreen_event *event);
	void handle_foreign_close();

	// Foreign toplevel handle, created on map.
	Handle<wlr_foreign_toplevel_handle_v1, wlr_foreign_toplevel_handle_v1_destroy> foreign_{};
	Listener foreign_activate_, foreign_maximize_, foreign_minimize_, foreign_fullscreen_,
		foreign_close_;
	// The ext-foreign-toplevel-list handle: created on map, closed on unmap,
	// its title and app id kept current (view.cpp).
	Handle<wlr_ext_foreign_toplevel_handle_v1,
		wlr_ext_foreign_toplevel_handle_v1_destroy> ext_foreign_{};

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
