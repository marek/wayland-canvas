#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include "canvas/desktop.h"
#include "windows/decoration.h"
#include "core/geometry.h"
#include "core/util.h"

class Output;
class Server;

// An xdg toplevel on the canvas.
class View {
public:
	View(Server &server, wlr_xdg_toplevel *wlr);
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
	void for_each_surface(std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const;
	// Foreign toplevel handle: the view entered or left `output`'s viewport.
	void foreign_output_enter(Output &output);
	void foreign_output_leave(Output &output);
	// The ext-foreign-toplevel-list handle's identifier, or null before the
	// window maps (or without the capture protocols). Window capture clients
	// (grim -T, the portal's window sharing) name windows by it.
	const char *ext_toplevel_identifier() const;

	Server &server;
	wlr_xdg_toplevel *const wlr;

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
	uint32_t resize_edges = 0; // edges being dragged; the opposite ones stay put
	// Configure that ended the resize: anchoring continues until the client acks it, since
	// its final resized commit arrives after the release.
	uint32_t resize_end_serial = 0;
	wlr_box resize_start{};
	uint32_t output_mask = 0; // outputs the view is visible on
	double told_scale = 0; // the scale its surfaces were last told: their outputs'
	uint64_t minimize_seq = 0;
	// Stable id for the IPC world (capture picks and sharing), assigned in
	// the constructor.
	uint64_t session_id = 0;

	Decoration deco;

private:
	void create_foreign();
	void destroy_foreign();

	void handle_map();
	void handle_unmap();
	void handle_commit();
	void handle_destroy();
	void handle_request_move();
	void handle_request_resize(wlr_xdg_toplevel_resize_event *event);
	void handle_request_maximize();
	void handle_request_minimize();
	void handle_request_fullscreen();
	void handle_set_title();
	void handle_set_app_id();
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
