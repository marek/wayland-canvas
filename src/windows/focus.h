#pragma once

#include <optional>
#include "core/util.h"
#include "windows/view.h"

class LayerSurface;
class Output;
class Server;

// Which window holds the focus, which shell surface holds the keyboard, and
// the hit-testing and stacking that choose them
// (docs/intents/windows/window-management.md).
class Focus {
public:
	explicit Focus(Server &server) : server_(server) {}
	Focus(const Focus &) = delete;
	Focus &operator=(const Focus &) = delete;

	View *focused_view() const { return view_; }
	// The keyboard-interactive layer surface that holds the keyboard: the
	// focused window has it when this is nullptr.
	LayerSurface *focused_layer() const { return layer_; }

	// What a canvas point lies on: the topmost window's surface there, or a
	// part of its title bar and border.
	std::optional<ViewHit> view_at(double cx, double cy, double zoom) const;
	// The window minimized most recently; nullptr when none is.
	View *last_minimized_view() const;

	// Activates and raises `view`; nullptr clears the focus.
	void focus_view(View *view);
	// Keyboard focus follows a move onto `box`: the focused window keeps it
	// when it stands there, else the topmost window there, the last used,
	// takes it (docs/intents/windows/window-management.md).
	void focus_in(wlr_box box);
	// The focused window went away or hid: the topmost window in sight of
	// the pointer's output takes the focus.
	void focus_topmost();
	// `view` was just minimized: it is numbered as the latest, and the focus
	// moves on when it held it.
	void view_minimized(View &view);
	// `view` or `layer` is going: the focus or the keyboard holder is cleared if it is that one.
	void forget(View &view);
	void forget(LayerSurface &layer);

	void focus_keyboard(wlr_surface *surface);
	// A shell surface that takes the keyboard, on map or by a click, gets it
	// and is recorded as the holder, so the keyboard goes back to the window
	// when the surface goes; an exclusive holder elsewhere keeps it.
	void offer_keyboard(LayerSurface &layer, wlr_surface *surface);
	// The keyboard back from an on-demand shell surface to the focused window.
	void return_keyboard();
	// An exclusive layer surface owns the keyboard while one is mapped; with
	// the holder gone, the keyboard goes back to the window.
	void refocus_layers();

private:
	View *top_visible_view(Output *output) const;

	Server &server_;
	View *view_ = nullptr;
	LayerSurface *layer_ = nullptr;
	uint64_t minimize_counter_ = 0;
};
