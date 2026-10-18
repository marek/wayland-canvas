#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "input/grab.h"
#include "core/settings.h"
#include "core/util.h"

class Output;
class Server;
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

	// canvas.conf was (re-)read: re-applies what the file decides about the
	// devices — the scroll direction (input/gestures.md's `invert`).
	void settings_changed();

	// A shortcut capture, for the settings window: from begin to end every
	// key, modifiers included, belongs to it — nothing runs and no surface
	// sees a key. `done` receives the chord pressed, or nullopt when it was
	// cancelled (Escape alone, or a second capture).
	void begin_shortcut_capture(
		std::function<void(std::optional<settings::Chord>)> done);
	// Ends the capture under way, if any; it answers nullopt.
	void cancel_shortcut_capture();

	// A button pressed on `surface` (or a surface of its tree) is still
	// held: the press its client answers by handing a drag to the
	// compositor. The press's layout position is where that drag starts.
	bool button_held_on(const wlr_surface *surface) const;
	double press_lx() const { return press_lx_; }
	double press_ly() const { return press_ly_; }

	// Title double-click state: the last click and its view.
	uint32_t last_title_click = 0;
	View *last_title_view = nullptr;
	const char *cursor_image = nullptr;

	PointerGrab grab;

	std::vector<std::unique_ptr<Keyboard>> keyboards;

	std::vector<std::unique_ptr<PointerDevice>> pointers;

	wlr_drag_icon *drag_icon = nullptr;
	double drag_icon_dx = 0, drag_icon_dy = 0;

private:
	Server &server_;

	// Presses swallowed by a binding or a compositor grab must have their
	// release swallowed too, or clients see stuck keys.
	std::array<bool, 256> key_consumed_{};
	// Set of buttons whose press was forwarded to a client, and where the
	// last such press was (layout coordinates).
	uint32_t pressed_buttons_ = 0;
	double press_lx_ = 0, press_ly_ = 0;
	// Logo key pressed alone and nothing else happened yet: releasing it
	// runs the action that holds the Super tap. Any other key, button,
	// scroll or gesture cancels the tap.
	bool super_tap_ = false;

	// A capture under way (shortcut.capture): every key belongs to it
	// until the chord is pressed or it is cancelled.
	struct ShortcutCapture {
		std::function<void(std::optional<settings::Chord>)> done;
		uint32_t mods = 0; // modifiers held inside the capture
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
	void forward_press(uint32_t time, uint32_t button);
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
