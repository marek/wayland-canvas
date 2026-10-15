#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "decoration.h"
#include "desktop.h"
#include "geometry.h"
#include "util.h"

class Output;
class Server;
class View;

enum class CursorMode {
	passthrough,
	move, // dragging a window
	resize,
	pan, // dragging empty canvas
	deco_button, // title bar button pressed, acts on release
	overview_press, // overview press, not yet a drag
	desktop_drag, // overview: moving a desktop frame
};

// What an overview press landed on.
enum class OverviewHit {
	empty,
	view,
	desktop,
	tab_tile,
	tab_abandon,
	shelf,
};

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

// Seat, cursor and keyboards: pointer focus, compositor grabs, keybindings, drag and drop.
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
	void cancel_grab();

	// Pointer grab state.
	CursorMode cursor_mode = CursorMode::passthrough;
	View *grab_view = nullptr;
	std::optional<DesktopId> grab_desktop;
	Output *grab_output = nullptr;
	OverviewHit grab_hit = OverviewHit::empty;
	double grab_lx = 0, grab_ly = 0; // layout coords where the grab started
	double grab_cx = 0, grab_cy = 0; // canvas coords where the grab started
	wlr_box grab_box{}; // view content box / desktop box at grab start
	Camera grab_cam;
	uint32_t grab_edges = 0;
	DecoPart grab_part = DecoPart::none;
	bool grab_moved = false;
	std::optional<DesktopId> drop_desktop; // combine target under a dragged frame
	uint32_t last_title_click = 0;
	View *last_title_view = nullptr;
	const char *cursor_image = nullptr;

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
	// Maximized-window drag: where the cursor sat on the title bar when the
	// drag started, so the restore can put it back at the same spot.
	double move_rel_x_ = 0, move_tb_off_ = 0;

	// Backend input devices.
	void handle_new_input(wlr_input_device *device);
	void handle_new_virtual_keyboard(wlr_virtual_keyboard_v1 *keyboard);
	void new_keyboard(wlr_input_device *device, bool has_keymap);
	void new_pointer(wlr_input_device *device);
	void update_capabilities();

	// Key events.
	void process_key(Keyboard &keyboard, wlr_keyboard_key_event *event);

	// Compositor grabs.
	void begin_move(View &view, Output &output);
	void begin_resize(View &view, Output &output, uint32_t edges);
	void begin_pan(Output &output);
	void begin_deco_button(View &view, Output &output, DecoPart part);
	void process_move_grab();
	void process_resize_grab();
	void process_pan_grab();
	void process_deco_button_grab();
	void process_desktop_drag_grab();
	void process_overview_press_grab();
	void process_grab_motion();
	void process_cursor_motion(uint32_t time);

	// Overview pointer interaction.
	void record_overview_hit(Output &output, double ox, double oy,
		double cx, double cy, double zoom);
	void overview_click();
	void overview_press(Output &output, uint32_t button);

	// Pointer buttons.
	void forward_press(uint32_t time, uint32_t button);
	void cursor_button_press(uint32_t time, uint32_t button);
	void cursor_button_release(uint32_t time, uint32_t button);
	void release_deco_button();

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
