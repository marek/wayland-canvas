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

// Which shell a window speaks. The canvas treats both alike; what differs
// — the configures, the close, the title — is the two files beside
// view.cpp: view_xdg.cpp and view_x11.cpp.
enum class ViewShell { xdg, x11 };

// A window on the canvas: an xdg toplevel, or an X11 window through
// Xwayland.
class View {
public:
	View(Server &server, wlr_xdg_toplevel *xdg);
	View(Server &server, wlr_xwayland_surface *x11);
	~View();
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
	// Every mapped surface of the view, with its position relative to the window geometry.
	void for_each_surface(
			std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const {
		is_x11() ? x11_for_each_surface(fn) : xdg_for_each_surface(fn);
	}
	// The surface at a point in the view's own coordinates, with the
	// point in that surface's coordinates; nullptr for a miss.
	wlr_surface *surface_at(double lx, double ly, double *sx, double *sy) const {
		return is_x11() ? x11_surface_at(lx, ly, sx, sy)
			: xdg_surface_at(lx, ly, sx, sy);
	}
	// Whether the client has acked the configure that ended a resize.
	bool resize_settled(uint32_t serial) const {
		return is_x11() ? x11_resize_settled(serial) : xdg_resize_settled(serial);
	}
	// The view whose xdg tree, or whose X11 window, holds `surface`.
	static View *from_xdg_surface(wlr_surface *surface);
	static View *from_x11_surface(wlr_surface *surface);
	// Foreign toplevel handle: the view entered or left `output`'s viewport.
	void foreign_output_enter(Output &output);
	void foreign_output_leave(Output &output);
	// The ext-foreign-toplevel-list handle's identifier, or null before the
	// window maps (or without the capture protocols). Window capture clients
	// (grim -T, the portal's window sharing) name windows by it.
	const char *ext_toplevel_identifier() const;

	// What the shared code asks of the window, whichever shell it speaks.
	// Each one is answered twice — in view_xdg.cpp and in view_x11.cpp —
	// and dispatched here, so view.cpp holds no shell-specific line.
	ViewShell shell() const { return x11 != nullptr ? ViewShell::x11 : ViewShell::xdg; }
	bool is_x11() const { return x11 != nullptr; }
	wlr_surface *surface() const {
		return is_x11() ? x11_surface() : xdg_surface();
	}
	wlr_box shell_geometry() const {
		return is_x11() ? x11_geometry() : xdg_geometry();
	}
	bool initialized() const {
		return is_x11() ? x11_initialized() : xdg_initialized();
	}
	void send_close() { is_x11() ? x11_send_close() : xdg_send_close(); }
	void configure_size(int width, int height) {
		is_x11() ? x11_configure_size(width, height)
			: xdg_configure_size(width, height);
	}
	void tell_maximized(bool want) {
		is_x11() ? x11_tell_maximized(want) : xdg_tell_maximized(want);
	}
	void tell_fullscreen(bool want) {
		is_x11() ? x11_tell_fullscreen(want) : xdg_tell_fullscreen(want);
	}
	void tell_tiled(bool want) {
		is_x11() ? x11_tell_tiled(want) : xdg_tell_tiled(want);
	}
	void tell_activated(bool want) {
		is_x11() ? x11_tell_activated(want) : xdg_tell_activated(want);
	}
	const char *shell_title() const {
		return is_x11() ? x11_title() : xdg_title();
	}
	const char *shell_app_id() const {
		return is_x11() ? x11_app_id() : xdg_app_id();
	}
	pid_t client_pid() const {
		return is_x11() ? x11_client_pid() : xdg_client_pid();
	}
	// What the window asked to open as.
	bool wants_maximized() const {
		return is_x11() ? x11_wants_maximized() : xdg_wants_maximized();
	}
	bool wants_fullscreen() const {
		return is_x11() ? x11_wants_fullscreen() : xdg_wants_fullscreen();
	}
	// The window it belongs to: an xdg toplevel's parent, an X11 window's
	// WM_TRANSIENT_FOR; nullptr for none.
	View *shell_parent() const {
		return is_x11() ? x11_parent() : xdg_parent();
	}
	// What the window is to the window manager; every layout rule reads it
	// (windows/view_kind.h). Modal by X11's _NET_WM_STATE_MODAL or by
	// xdg-dialog-v1.
	ViewKind kind() const {
		if (override_redirect) {
			return ViewKind::client_placed;
		}
		if (is_x11() ? x11_modal() : xdg_modal()) {
			return ViewKind::modal;
		}
		return shell_parent() != nullptr ? ViewKind::dialog : ViewKind::normal;
	}

	Server &server;
	// Exactly one of the two is set; `shell()` says which.
	wlr_xdg_toplevel *const wlr = nullptr;
	wlr_xwayland_surface *const x11 = nullptr;

	// Canvas position of the window geometry's top-left corner.
	int x = 0, y = 0;
	bool mapped = false, minimized = false, maximized = false, fullscreen = false,
		tiled = false;
	// An X11 override-redirect window: a menu, a tooltip or a drag icon,
	// which the window manager must not decorate, list or place
	// (docs/intents/windows/x11.md). Always false for a wayland window.
	bool override_redirect = false;
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
	uint32_t resize_edges = 0; // edges being dragged; the opposite ones stay put
	// Configure that ended the resize: anchoring continues until the client acks it, since
	// its final resized commit arrives after the release.
	uint32_t resize_end_serial = 0;
	wlr_box resize_start{};
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

private:
	void create_foreign();
	void destroy_foreign();

	// view_xdg.cpp
	wlr_surface *xdg_surface() const;
	wlr_box xdg_geometry() const;
	bool xdg_initialized() const;
	void xdg_send_close();
	void xdg_configure_size(int width, int height);
	void xdg_tell_maximized(bool want);
	void xdg_tell_fullscreen(bool want);
	void xdg_tell_tiled(bool want);
	void xdg_tell_activated(bool want);
	const char *xdg_title() const;
	const char *xdg_app_id() const;
	pid_t xdg_client_pid() const;
	bool xdg_wants_maximized() const;
	bool xdg_wants_fullscreen() const;
	bool xdg_resize_settled(uint32_t serial) const;
	View *xdg_parent() const;
	bool xdg_modal() const;
	void xdg_for_each_surface(
		std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const;
	wlr_surface *xdg_surface_at(double lx, double ly, double *sx, double *sy) const;
	void handle_xdg_commit();
	void handle_xdg_request_maximize();
	void handle_xdg_request_minimize();
	void handle_xdg_request_fullscreen();

	// view_x11.cpp
	void x11_place_override_redirect();
	// Where X has the window: its canvas position less the shared offset.
	int16_t x11_x() const;
	int16_t x11_y() const;
	// Tells X the window's canvas place, when X has it elsewhere; true when
	// it did.
	bool x11_follow_canvas();
	wlr_surface *x11_surface() const;
	wlr_box x11_geometry() const;
	bool x11_initialized() const;
	void x11_send_close();
	void x11_configure_size(int width, int height);
	void x11_tell_maximized(bool want);
	void x11_tell_fullscreen(bool want);
	void x11_tell_tiled(bool want);
	void x11_tell_activated(bool want);
	const char *x11_title() const;
	const char *x11_app_id() const;
	pid_t x11_client_pid() const;
	bool x11_wants_maximized() const;
	bool x11_wants_fullscreen() const;
	View *x11_parent() const;
	bool x11_modal() const;
	bool x11_resize_settled(uint32_t serial) const;
	void x11_for_each_surface(
		std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const;
	wlr_surface *x11_surface_at(double lx, double ly, double *sx, double *sy) const;

	// The shared half of the life cycle (view.cpp); the two shells' own
	// listeners call into these.
	void handle_map();
	void handle_unmap();
	void handle_first_commit(); // the client's very first commit
	void handle_commit();
	void handle_destroy();
	void handle_request_move();
	void handle_request_resize(wlr_xdg_toplevel_resize_event *event);
	void handle_set_title();
	void handle_set_app_id();
	// view_x11.cpp's own listeners.
	void handle_x11_associate();
	void handle_x11_dissociate();
	void handle_x11_configure(wlr_xwayland_surface_configure_event *event);
	void handle_x11_set_geometry();
	void handle_x11_request_maximize();
	void handle_x11_request_minimize();
	void handle_x11_request_fullscreen();
	// Connects the listeners of whichever shell this window speaks.
	void listen_xdg();
	void listen_x11();
	void handle_foreign_activate();
	void handle_foreign_maximize(const wlr_foreign_toplevel_handle_v1_maximized_event *event);
	void handle_foreign_minimize(const wlr_foreign_toplevel_handle_v1_minimized_event *event);
	void handle_foreign_fullscreen(const wlr_foreign_toplevel_handle_v1_fullscreen_event *event);
	void handle_foreign_close();

	// Foreign toplevel handle, created on map.
	Handle<wlr_foreign_toplevel_handle_v1, wlr_foreign_toplevel_handle_v1_destroy> foreign_{};
	Listener map_, unmap_, commit_, destroy_;
	Listener request_move_, request_resize_, request_maximize_, request_minimize_,
		request_fullscreen_;
	Listener set_title_, set_app_id_;
	// X11 only: the surface coming and going, and its own requests.
	Listener x11_associate_, x11_dissociate_, x11_configure_, x11_set_geometry_;
	Listener foreign_activate_, foreign_maximize_, foreign_minimize_, foreign_fullscreen_,
		foreign_close_;
	// The ext-foreign-toplevel-list handle: created on map, closed on unmap,
	// its title and app id kept current (view.cpp).
	Handle<wlr_ext_foreign_toplevel_handle_v1,
		wlr_ext_foreign_toplevel_handle_v1_destroy> ext_foreign_{};

	friend class Server;
};

// An xdg popup; its surface tree belongs to its parent's.
class Popup {
public:
	Popup(Server &server, wlr_xdg_popup *wlr);
	~Popup();
	Popup(const Popup &) = delete;
	Popup &operator=(const Popup &) = delete;

	wlr_xdg_popup *const wlr;

private:
	void handle_commit();
	void handle_destroy();

	Server &server_;
	Listener commit_, destroy_;
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
