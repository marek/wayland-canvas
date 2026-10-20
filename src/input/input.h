#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "input/grab.h"
#include "input/tablet.h"
#include "input/touch.h"
#include "core/settings.h"
#include "core/util.h"

class LayerSurface;
class Output;
class Server;
class UiLayer;
class View;
struct libinput_device;

struct CanvasPos {
	Output *output = nullptr;
	double cx = 0, cy = 0;
};

class Keyboard {
public:
	// `has_keymap`: keep the keymap the device carries (virtual keyboards)
	// instead of applying the environment's xkbcommon defaults.
	Keyboard(Server &server, wlr_keyboard *wlr, bool has_keymap);
	~Keyboard();
	Keyboard(const Keyboard &) = delete;
	Keyboard &operator=(const Keyboard &) = delete;

	wlr_keyboard *const wlr;

private:
	void handle_modifiers();
	void handle_key(wlr_keyboard_key_event *event);
	void handle_destroy();

	Server &server_;
	Listener modifiers_, key_, destroy_;
};

class Input;

// One libinput pointer the backend handed over, kept so a settings change
// reaches the devices already here; it goes with its device.
class PointerDevice {
public:
	PointerDevice(Input &input, wlr_input_device *device, libinput_device *dev);
	PointerDevice(const PointerDevice &) = delete;
	PointerDevice &operator=(const PointerDevice &) = delete;

	libinput_device *const dev;

private:
	void handle_destroy();

	Input &input_;
	Listener destroy_;
};

// Seat, cursor and keyboards: pointer focus, keybindings, drag and drop.
// The compositor-owned pointer interaction in progress is `grab`.
class Input {
public:
	explicit Input(Server &server);
	~Input();
	Input(const Input &) = delete;
	Input &operator=(const Input &) = delete;

	void init(); // listens to the backend's input devices, the cursor and the seat
	void finish(); // disconnects before the seat and cursor are destroyed

	// The canvas moved under the pointer: refresh pointer focus and hover.
	void rebase_cursor();
	std::optional<CanvasPos> cursor_canvas_pos() const;
	void set_cursor_image(const char *name);
	// A view was unmapped or destroyed: drop what referred to it.
	void forget(const View &view);

	// ---- the compositor's own UIs (input/ui_layer.h) ----

	// Set once by Server: the layers, topmost first. Every path that gives
	// a layer input, a frame or a change of palette goes through this list.
	void set_layers(std::vector<UiLayer *> layers) { layers_ = std::move(layers); }
	std::span<UiLayer *const> layers() const { return layers_; }
	// The topmost layer that owns the keyboard: it gets every key press.
	UiLayer *layer_for_keys() const;
	// The topmost layer that takes the pointer, a touch or the pen at
	// output-local (ox, oy) on `output`: it gets the event, and no window
	// or surface does.
	UiLayer *layer_at(const Output &output, double ox, double oy) const;
	// Whether a layer owns touchpad and touchscreen swipes and pinches.
	bool gestures_blocked() const;

	// canvas.conf was (re-)read: re-applies what the file decides about the
	// devices — the mouse wheel's and the touchpad's scroll direction
	// (input/gestures.md).
	void settings_changed();

	// A shortcut capture, for the settings window: from begin to end every
	// key, modifiers included, belongs to it — nothing runs and no surface
	// sees a key. `done` receives the chord pressed, or nullopt when it was
	// cancelled (Escape alone, or a second capture).
	void begin_shortcut_capture(
		std::function<void(std::optional<settings::Chord>)> done);
	// Ends the capture under way, if any; it answers nullopt.
	void cancel_shortcut_capture();
	// Drops a tap waiting to be told from a double tap, unrun: the session
	// locking, a capture beginning.
	void cancel_pending_tap();

	// A button pressed on `surface` (or a surface of its tree) is still
	// held: the press its client answers by handing a drag to the
	// compositor. The press's layout position is where that drag starts.
	bool button_held_on(const wlr_surface *surface) const;
	double press_lx() const { return held_.lx; }
	double press_ly() const { return held_.ly; }
	// The pointer is no longer kept on the held surface; the next motion
	// finds what is under it. The buttons stay as they are.
	void end_hold();
	// Every touch contact and every pen in range ends as if lifted: the
	// session locking or an output going, so none goes on reaching what it
	// reached before.
	void end_contacts();

	// Title double-click state: the last click and its view.
	uint32_t last_title_click = 0;
	View *last_title_view = nullptr;
	const char *cursor_image = nullptr;

	PointerGrab grab;

	std::vector<std::unique_ptr<Keyboard>> keyboards;

	std::vector<std::unique_ptr<PointerDevice>> pointers;

	wlr_drag_icon *drag_icon = nullptr;
	double drag_icon_dx = 0, drag_icon_dy = 0;

	// ---- touch and the pen (input/touch.cpp, input/tablet.cpp) ----

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
	// What Super and a scroll do, by (dx, dy) screen px: the view looks
	// away, or in the overview its camera pans.
	void look_away(Output &output, double dx, double dy);
	// Touch and pen input are activity: the idle countdown restarts and a
	// Super tap is off.
	void contact_activity();
	// The output a touchscreen or a pen acts on: the one it names, else the
	// built-in panel, else the first.
	Output *output_for_device(const char *output_name);
	// The seat offers touch while a touchscreen is connected.
	void update_capabilities();
	// Outputs came or went: every touchscreen and tablet is mapped again.
	void outputs_changed();

	// ---- input that waits for X (docs/intents/windows/x11.md) ----
	// Whether `event` waits, to run through `Handler` later and in order:
	// one waits already, or it is for an X11 window (`for_x`: a press, a
	// scroll, a touch or a pen tip there) while X still has window changes
	// to take in.
	template <auto Handler, typename Owner, typename Event>
	bool waits(Owner *owner, const Event *event, bool for_x);
	// The same for an event without data: a frame.
	template <auto Handler, typename Owner>
	bool waits(Owner *owner);
	// Whatever waits runs now, in order: before a device goes, since a
	// waiting event names it.
	void run_waiting();

	Touch touch;
	Tablets tablets;

private:
	Server &server_;
	std::vector<UiLayer *> layers_; // topmost first

	bool must_wait(bool for_x);
	void queue(std::function<void()> run);
	// Runs what waits, in order, until a press for X finds X still behind
	// (it goes first again) or, `forced`, everything.
	void release_waiting(bool forced);
	static int handle_x_wait(void *data);
	std::deque<std::function<void()>> waiting_;
	bool releasing_ = false, requeued_ = false, forced_ = false;
	EventSourcePtr x_wait_timer_;
	double x_wait_since_ = 0;

	// Presses swallowed by a binding or a compositor grab must have their
	// release swallowed too, or clients see stuck keys.
	std::array<bool, 256> key_consumed_{};
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
	void handle_held_destroy();
	// Motion while a press is held: to the held surface, wherever the
	// cursor is. False when nothing holds the pointer this way.
	bool held_motion(uint32_t time);
	// Logo key pressed alone and nothing else happened yet: releasing it
	// runs the action that holds the Super tap. Any other key, button,
	// scroll or gesture cancels the tap.
	bool super_tap_ = false;
	// A lone tap waiting out the double-tap interval, since an action holds
	// the double tap; the timer runs it when no second tap comes.
	bool tap_pending_ = false;
	EventSourcePtr tap_timer_;
	void other_input();
	void lone_tap();
	void run_pending_tap();
	void arm_tap_timer();
	static int handle_tap_timer(void *data);

	// A capture under way (shortcut.capture): every key belongs to it
	// until the chord is pressed or it is cancelled.
	struct ShortcutCapture {
		std::function<void(std::optional<settings::Chord>)> done;
		uint32_t mods = 0; // modifiers held inside the capture
		bool tapped = false; // a lone Super tap: one, or the first of two
		bool ending = false; // answered; the keys held at the end drain
		std::vector<uint32_t> draining; // their keycodes
	};
	std::optional<ShortcutCapture> shortcut_capture_;


	// Backend input devices.
	void handle_new_input(wlr_input_device *device);
	void handle_new_virtual_keyboard(wlr_virtual_keyboard_v1 *keyboard);
	void new_keyboard(wlr_input_device *device, bool has_keymap);
	void new_pointer(wlr_input_device *device);
	// The settings' share of a device's libinput configuration.
	void apply_pointer_settings(libinput_device *dev);

	// Key events.
	void process_key(Keyboard &keyboard, wlr_keyboard_key_event *event);
	// Answers the capture — the chord pressed, or nothing on a cancel —
	// after which the keys held at the end drain before it is gone.
	void end_shortcut_capture(std::optional<settings::Chord> chord);
	// Every key of a shortcut capture: modifiers are tracked, the first
	// non-modifier key ends it as the chord, a lone Super release is the
	// tap, and a bare Escape cancels.
	void capture_key(Keyboard &keyboard, wlr_keyboard_key_event *event,
		std::span<const xkb_keysym_t> syms);

	// Pointer motion and buttons.
	void process_cursor_motion(uint32_t time);
	// True while the session is locked: the pointer went to the locker's
	// surface for the output it is over, or to nothing.
	bool locked_pointer_focus(uint32_t time);
	// A press to the client of `surface`, at (sx, sy) on it; `scale` is how
	// many surface px one layout px is there.
	void forward_press(uint32_t time, uint32_t button, wlr_surface *surface,
		double sx, double sy, double scale);
	void cursor_button_press(uint32_t time, uint32_t button);
	void cursor_button_release(uint32_t time, uint32_t button);
	// A press while the output is in the overview.
	void overview_press(Output &output, uint32_t time, uint32_t button);

	// Cursor events.
	void handle_cursor_motion(wlr_pointer_motion_event *event);
	void handle_cursor_motion_absolute(wlr_pointer_motion_absolute_event *event);
	void handle_cursor_button(wlr_pointer_button_event *event);
	void handle_cursor_axis(wlr_pointer_axis_event *event);
	void handle_cursor_frame();

	// Touchpad gestures (recognition lives in gesture.cpp).
	void handle_swipe_begin(wlr_pointer_swipe_begin_event *event);
	void handle_swipe_update(wlr_pointer_swipe_update_event *event);
	void handle_swipe_end(wlr_pointer_swipe_end_event *event);
	void handle_pinch_begin(wlr_pointer_pinch_begin_event *event);
	void handle_pinch_update(wlr_pointer_pinch_update_event *event);
	void handle_pinch_end(wlr_pointer_pinch_end_event *event);
	void handle_hold_begin(wlr_pointer_hold_begin_event *event);
	void handle_hold_end(wlr_pointer_hold_end_event *event);

	// Seat requests, drag and drop, selection.
	void handle_request_cursor(wlr_seat_pointer_request_set_cursor_event *event);
	void handle_request_set_shape(
		wlr_cursor_shape_manager_v1_request_set_shape_event *event);
	void handle_request_set_selection(wlr_seat_request_set_selection_event *event);
	void handle_request_set_primary_selection(
		wlr_seat_request_set_primary_selection_event *event);
	void handle_request_start_drag(wlr_seat_request_start_drag_event *event);
	void handle_start_drag(wlr_drag *drag);
	void handle_drag_icon_commit(wlr_surface *surface);
	void handle_drag_icon_destroy();

	Listener new_input_, new_virtual_keyboard_, request_cursor_shape_;
	Listener cursor_motion_, cursor_motion_absolute_, cursor_button_,
		cursor_axis_, cursor_frame_;
	Listener swipe_begin_, swipe_update_, swipe_end_;
	Listener pinch_begin_, pinch_update_, pinch_end_;
	Listener hold_begin_, hold_end_;
	Listener request_cursor_, request_set_selection_,
		request_set_primary_selection_;
	Listener request_start_drag_, start_drag_;
	Listener drag_icon_commit_, drag_icon_destroy_;

	friend class Keyboard;
};

template <auto Handler, typename Owner, typename Event>
bool Input::waits(Owner *owner, const Event *event, bool for_x) {
	if (!must_wait(for_x)) {
		return false;
	}
	queue([owner, copy = *event]() mutable { (owner->*Handler)(&copy); });
	return true;
}

template <auto Handler, typename Owner>
bool Input::waits(Owner *owner) {
	if (!must_wait(false)) {
		return false;
	}
	queue([owner] { (owner->*Handler)(); });
	return true;
}
