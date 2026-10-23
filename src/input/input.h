#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "input/drag_and_drop.h"
#include "input/grab.h"
#include "input/pointer_router.h"
#include "input/shortcut_capture.h"
#include "input/super_tap.h"
#include "input/tablet.h"
#include "input/touch.h"
#include "input/x_input_gate.h"
#include "core/util.h"

class InputGate;
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

// The seat and its devices: keyboards, pointers, touchscreens and tablets
// as the backend hands them over, and the events they bring, each handed to
// what acts on it — the key bindings, the pointer's router, the gestures,
// touch and the pen. The compositor-owned pointer interaction in progress is
// `grab`.
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
	// The desktop under the cursor when the cursor is on `output`, else null.
	Desktop *desktop_under_cursor(const Output &output) const;
	// The binding modifiers (Shift, Ctrl, Alt, Super) held on the seat's
	// keyboard, as WLR_MODIFIER_* bits.
	uint32_t held_modifiers() const;
	bool super_held() const;
	void set_cursor_image(const char *name);
	// A view was unmapped or destroyed: drop what referred to it.
	void forget(const View &view);

	// Set once by Server: the layers, topmost first. Every path that gives
	// a layer input, a frame or a change of palette goes through this list.
	void set_layers(std::vector<UiLayer *> layers) { layers_ = std::move(layers); }
	std::span<UiLayer *const> layers() const { return layers_; }
	// Set once by Server: the session lock, which says whether the session
	// is locked and which surface of the locker takes input then.
	void set_gate(InputGate &gate) { gate_ = &gate; }
	InputGate &gate() const { return *gate_; }
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

	// The session locking: every interaction under way ends without running
	// what it was for — a pointer grab, the press held on a client, every
	// touch contact and pen in range, a shortcut capture, a Super tap waiting
	// out its interval.
	void abandon_interaction();
	// Every touch contact and every pen in range ends as if lifted: the
	// session locking or an output going, so none goes on reaching what it
	// reached before.
	void end_contacts();

	PointerGrab grab;
	XInputGate x_gate;
	SuperTap super_tap;
	ShortcutCapture shortcut_capture;
	PointerRouter router;
	DragAndDrop dnd;

	std::vector<std::unique_ptr<Keyboard>> keyboards;

	std::vector<std::unique_ptr<PointerDevice>> pointers;

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
	// `output` is about to be destroyed: every contact ends, a grab on it is
	// cancelled, and the compositor's UIs on it close.
	void output_gone(Output &output);

	Touch touch;
	Tablets tablets;

private:
	Server &server_;
	std::vector<UiLayer *> layers_; // topmost first
	InputGate *gate_ = nullptr;
	// The name of the cursor image set, or null while a client owns it.
	const char *cursor_image_ = nullptr;

	// Presses swallowed by a binding or a compositor grab must have their
	// release swallowed too, or clients see stuck keys.
	std::array<bool, 256> key_consumed_{};

	// Backend input devices.
	void handle_new_input(wlr_input_device *device);
	void handle_new_virtual_keyboard(wlr_virtual_keyboard_v1 *keyboard);
	void new_keyboard(wlr_input_device *device, bool has_keymap);
	void new_pointer(wlr_input_device *device);
	// The settings' share of a device's libinput configuration.
	void apply_pointer_settings(libinput_device *dev);

	// Key events.
	void process_key(Keyboard &keyboard, wlr_keyboard_key_event *event);

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

	// Seat requests, selection.
	void handle_request_cursor(wlr_seat_pointer_request_set_cursor_event *event);
	void handle_request_set_shape(
		wlr_cursor_shape_manager_v1_request_set_shape_event *event);
	void handle_request_set_selection(wlr_seat_request_set_selection_event *event);
	void handle_request_set_primary_selection(
		wlr_seat_request_set_primary_selection_event *event);

	Listener new_input_, new_virtual_keyboard_, request_cursor_shape_;
	Listener cursor_motion_, cursor_motion_absolute_, cursor_button_,
		cursor_axis_, cursor_frame_;
	Listener swipe_begin_, swipe_update_, swipe_end_;
	Listener pinch_begin_, pinch_update_, pinch_end_;
	Listener hold_begin_, hold_end_;
	Listener request_cursor_, request_set_selection_,
		request_set_primary_selection_;

	friend class Keyboard;
};
