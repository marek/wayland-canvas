#pragma once

#include <cstdint>
#include <optional>
#include "core/util.h"

class Input;
class LayerSurface;
class Output;
class Server;
class View;

// Where the pointer goes, and a touch or the pen acting as it: pointer
// focus and hover, which layer, shell surface, window frame or client gets a
// press, a scroll or a motion, and the implicit grab that keeps a pressed
// surface the pointer until its buttons are up
// (docs/intents/input/keyboard-and-pointer.md).
class PointerRouter {
public:
	PointerRouter(Server &server, Input &input);
	PointerRouter(const PointerRouter &) = delete;
	PointerRouter &operator=(const PointerRouter &) = delete;

	// The pointer moved, or the canvas under it did: pointer focus and
	// hover go to what is under it now.
	void motion(uint32_t time);
	void button_press(uint32_t time, uint32_t button);
	void button_release(uint32_t time, uint32_t button);
	void axis(const wlr_pointer_axis_event &event);
	// What Super and a scroll do, by (dx, dy) screen px: the view looks
	// away, or in the overview its camera pans.
	void look_away(Output &output, double dx, double dy);

	// A button pressed on `surface` (or a surface of its tree) is still
	// held: the press its client answers by handing a drag to the
	// compositor. The press's layout position is where that drag starts.
	bool button_held_on(const wlr_surface *surface) const;
	double press_lx() const { return held_.lx; }
	double press_ly() const { return held_.ly; }
	// The pointer is no longer kept on the held surface; the next motion
	// finds what is under it. The buttons stay as they are.
	void end_hold();

	// A view was unmapped or destroyed: drop what referred to it.
	void forget(const View &view);
	// Title double-click state: the view of the last click.
	View *last_title_view = nullptr;

	// What a touch or the pen at layout (lx, ly) reaches of the clients'
	// surfaces, by the pointer's own precedence: the locker while the
	// session is locked, a shell surface, a window, a lower layer. Nothing
	// over the compositor's own UI and the empty canvas, which take a
	// contact as the pointer (docs/intents/input/touch-and-pen.md).
	struct ContactHit {
		wlr_surface *surface = nullptr;
		double sx = 0, sy = 0; // where on it
		double scale = 1; // its px per layout px
		View *view = nullptr; // the window it belongs to, if any
		LayerSurface *layer = nullptr; // or the shell surface
	};
	std::optional<ContactHit> client_at(double lx, double ly);
	// The canvas itself at (lx, ly), where two fingers move the view: in
	// the normal view no window, title bar or surface is there; in the
	// overview anything but a shell surface.
	bool canvas_at(double lx, double ly);
	// A contact landing on `hit` as a click would: its window is focused
	// and raised, its shell surface given the keyboard if it takes it.
	void contact_focus(const ContactHit &hit);
	// A contact driving the pointer: the cursor goes to (lx, ly) and acts
	// there as the mouse does; its buttons are the mouse's.
	void contact_motion(uint32_t time, double lx, double ly);
	void contact_button(uint32_t time, uint32_t button, bool pressed);

private:
	// True while the session is locked: the pointer went to the locker's
	// surface for the output it is over, or to nothing.
	bool locked_pointer_focus(uint32_t time);
	// A press to the client of `surface`, at (sx, sy) on it; `scale` is how
	// many surface px one layout px is there.
	void forward_press(uint32_t time, uint32_t button, wlr_surface *surface,
		double sx, double sy, double scale);
	// Motion while a press is held: to the held surface, wherever the
	// cursor is. False when nothing holds the pointer this way.
	bool held_motion(uint32_t time);
	void handle_held_destroy();
	// A press while the output is in the overview.
	void overview_press(Output &output, uint32_t button);

	Server &server_;
	Input &input_;
	uint32_t last_title_click_ = 0;
	// Set of buttons whose press was forwarded to a client.
	uint32_t pressed_buttons_ = 0;
	// The surface the first of those presses reached keeps the pointer
	// until the last is released (Wayland's implicit grab): where on it the
	// press was, in its own coordinates, where the cursor was then (layout
	// px), and how many surface px a layout px is (1/zoom on the plane).
	struct HeldPress {
		wlr_surface *surface = nullptr;
		double sx = 0, sy = 0, lx = 0, ly = 0, scale = 1;
	};
	HeldPress held_;
	Listener held_destroy_;
};
