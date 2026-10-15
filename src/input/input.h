#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "input/grab.h"
#include "core/util.h"

class Output;
class Server;
class View;

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

	// Title double-click state: the last click and its view.
	uint32_t last_title_click = 0;
	View *last_title_view = nullptr;
	const char *cursor_image = nullptr;

	PointerGrab grab;

	std::vector<std::unique_ptr<Keyboard>> keyboards;

	wlr_drag_icon *drag_icon = nullptr;
	double drag_icon_dx = 0, drag_icon_dy = 0;

private:
	Server &server_;

	// Presses swallowed by a binding or a compositor grab must have their
	// release swallowed too, or clients see stuck keys.
	std::array<bool, 256> key_consumed_{};
	// Set of buttons whose press was forwarded to a client.
	uint32_t pressed_buttons_ = 0;
	// Logo key pressed alone and nothing else happened yet: releasing it opens
	// the launcher. Any other key, button, scroll or gesture cancels the tap.
	bool super_tap_ = false;

	// Backend input devices.
	void handle_new_input(wlr_input_device *device);
	void handle_new_virtual_keyboard(wlr_virtual_keyboard_v1 *keyboard);
	void new_keyboard(wlr_input_device *device, bool has_keymap);
	void new_pointer(wlr_input_device *device);
	void update_capabilities();

	// Key events.
	void process_key(Keyboard &keyboard, wlr_keyboard_key_event *event);

	// Pointer motion and buttons.
	void process_cursor_motion(uint32_t time);
	void forward_press(uint32_t time, uint32_t button);
	void cursor_button_press(uint32_t time, uint32_t button);
	void cursor_button_release(uint32_t time, uint32_t button);
	// A press while the output is in the overview.
	void overview_press(Output &output, uint32_t button);

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
