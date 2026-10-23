#include "harness/devices.h"

#include <optional>
#include <ranges>
#include <utility>
#include "harness/run.h"
#include "platform/output.h"

namespace harness {

namespace {

uint32_t now = 90000;
bool real_time = false;

wlr_pointer pointer_device{};
const wlr_pointer_impl pointer_impl = {.name = "fake-pointer"};
wlr_keyboard keyboard_device{};
const wlr_keyboard_impl keyboard_impl = {.name = "fake-keyboard"};
wlr_touch touch_device{};
const wlr_touch_impl touch_impl = {.name = "fake-touchscreen"};
wlr_tablet tablet_device{};
const wlr_tablet_impl tablet_impl = {.name = "fake-tablet"};
wlr_tablet_tool pen{};

bool has_pointer = false, has_keyboard = false, has_touch = false, has_tablet = false;

// Fractions of the first output's logical size, as a device reports them.
double fraction_x(double x) {
	return x / output().width;
}

double fraction_y(double y) {
	return y / output().height;
}

} // namespace

uint32_t event_time() {
	if (real_time) {
		return static_cast<uint32_t>(now_ms());
	}
	now += 8;
	return now;
}

void advance_clock(uint32_t ms) {
	now += ms;
}

void use_real_time() {
	real_time = true;
}

void attach_keyboard() {
	wlr_keyboard_init(&keyboard_device, &keyboard_impl, "fake-keyboard");
	wl_signal_emit_mutable(&server->backend->events.new_input, &keyboard_device.base);
	has_keyboard = true;
}

void attach_pointer() {
	wlr_pointer_init(&pointer_device, &pointer_impl, "fake-pointer");
	wl_signal_emit_mutable(&server->backend->events.new_input, &pointer_device.base);
	has_pointer = true;
}

void attach_touchscreen() {
	wlr_touch_init(&touch_device, &touch_impl, "fake-touchscreen");
	wl_signal_emit_mutable(&server->backend->events.new_input, &touch_device.base);
	has_touch = true;
}

void attach_tablet() {
	wlr_tablet_init(&tablet_device, &tablet_impl, "fake-tablet");
	wl_signal_emit_mutable(&server->backend->events.new_input, &tablet_device.base);
	pen.type = WLR_TABLET_TOOL_TYPE_PEN;
	pen.pressure = true;
	pen.tilt = true;
	wl_signal_init(&pen.events.destroy);
	has_tablet = true;
}

wlr_touch &touchscreen() {
	return touch_device;
}

void detach_devices() {
	if (has_tablet) {
		wl_signal_emit_mutable(&pen.events.destroy, &pen);
		wlr_tablet_finish(&tablet_device);
	}
	if (has_touch) {
		wlr_touch_finish(&touch_device);
	}
	if (has_pointer) {
		wlr_pointer_finish(&pointer_device);
	}
	if (has_keyboard) {
		wlr_keyboard_finish(&keyboard_device);
	}
	has_pointer = has_keyboard = has_touch = has_tablet = false;
}

// ------------------------------------------------------------ keyboard

void key(uint32_t code, bool pressed) {
	wlr_keyboard_key_event event = {.time_msec = event_time(), .keycode = code,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&keyboard_device, &event);
}

void tap(uint32_t code) {
	key(code, true);
	key(code, false);
}

void chord(uint32_t code, std::span<const uint32_t> modifiers) {
	for (uint32_t held : modifiers) {
		key(held, true);
	}
	tap(code);
	for (uint32_t held : std::views::reverse(modifiers)) {
		key(held, false);
	}
}

namespace {

struct Stroke {
	uint32_t code;
	bool shift;
};

std::optional<Stroke> stroke_for(char c) {
	static constexpr uint32_t letters[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
		KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S,
		KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
	static constexpr uint32_t digits[] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
		KEY_7, KEY_8, KEY_9};
	if (c >= 'a' && c <= 'z') {
		return Stroke{letters[c - 'a'], false};
	}
	if (c >= 'A' && c <= 'Z') {
		return Stroke{letters[c - 'A'], true};
	}
	if (c >= '0' && c <= '9') {
		return Stroke{digits[c - '0'], false};
	}
	struct Punctuation {
		char c;
		Stroke stroke;
	};
	static constexpr Punctuation punctuation[] = {
		{' ', {KEY_SPACE, false}}, {'\n', {KEY_ENTER, false}}, {'-', {KEY_MINUS, false}},
		{'_', {KEY_MINUS, true}}, {'=', {KEY_EQUAL, false}}, {'+', {KEY_EQUAL, true}},
		{'/', {KEY_SLASH, false}}, {'?', {KEY_SLASH, true}}, {'.', {KEY_DOT, false}},
		{'>', {KEY_DOT, true}}, {',', {KEY_COMMA, false}}, {'<', {KEY_COMMA, true}},
		{';', {KEY_SEMICOLON, false}}, {':', {KEY_SEMICOLON, true}},
		{'\'', {KEY_APOSTROPHE, false}}, {'"', {KEY_APOSTROPHE, true}},
		{'(', {KEY_9, true}}, {')', {KEY_0, true}}, {'!', {KEY_1, true}},
		{'%', {KEY_5, true}}, {'*', {KEY_8, true}}, {'&', {KEY_7, true}},
		{'#', {KEY_3, true}}, {'@', {KEY_2, true}}, {'[', {KEY_LEFTBRACE, false}},
		{']', {KEY_RIGHTBRACE, false}}, {'{', {KEY_LEFTBRACE, true}},
		{'}', {KEY_RIGHTBRACE, true}},
	};
	for (const Punctuation &p : punctuation) {
		if (p.c == c) {
			return p.stroke;
		}
	}
	return std::nullopt;
}

} // namespace

void type_char(char c) {
	std::optional<Stroke> stroke = stroke_for(c);
	if (!stroke.has_value()) {
		return;
	}
	if (stroke->shift) {
		key(KEY_LEFTSHIFT, true);
	}
	tap(stroke->code);
	if (stroke->shift) {
		key(KEY_LEFTSHIFT, false);
	}
}

void type_text(std::string_view text) {
	for (char c : text) {
		type_char(c);
	}
}

// ------------------------------------------------------------- pointer

void move_to(double x, double y) {
	wlr_pointer_motion_absolute_event event = {.pointer = &pointer_device,
		.time_msec = event_time(), .x = fraction_x(x), .y = fraction_y(y)};
	wl_signal_emit_mutable(&pointer_device.events.motion_absolute, &event);
	wl_signal_emit_mutable(&pointer_device.events.frame, &pointer_device);
}

void button(uint32_t code, bool pressed) {
	wlr_pointer_button_event event = {.pointer = &pointer_device, .time_msec = event_time(),
		.button = code,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&pointer_device.events.button, &event);
	wl_signal_emit_mutable(&pointer_device.events.frame, &pointer_device);
}

void click(double x, double y, uint32_t code) {
	move_to(x, y);
	button(code, true);
	button(code, false);
}

void drag(double x0, double y0, double x1, double y1) {
	move_to(x0, y0);
	button(BTN_LEFT, true);
	for (int i = 1; i <= 10; i++) {
		move_to(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
	}
	button(BTN_LEFT, false);
}

void swipe(int fingers, double dx, double dy) {
	server->gestures.swipe_begin(event_time(), static_cast<uint32_t>(fingers));
	for (int i = 0; i < 10; i++) {
		server->gestures.swipe_update(event_time(), dx / 10, dy / 10);
	}
	server->gestures.swipe_end(event_time(), false);
}

void touchpad_swipe_begin(int fingers) {
	wlr_pointer_swipe_begin_event event = {.pointer = &pointer_device,
		.time_msec = event_time(), .fingers = static_cast<uint32_t>(fingers)};
	wl_signal_emit_mutable(&pointer_device.events.swipe_begin, &event);
}

void touchpad_swipe_update(int fingers, double dx, double dy) {
	wlr_pointer_swipe_update_event event = {.pointer = &pointer_device,
		.time_msec = event_time(), .fingers = static_cast<uint32_t>(fingers), .dx = dx,
		.dy = dy};
	wl_signal_emit_mutable(&pointer_device.events.swipe_update, &event);
}

void touchpad_swipe_end() {
	wlr_pointer_swipe_end_event event = {.pointer = &pointer_device,
		.time_msec = event_time(), .cancelled = false};
	wl_signal_emit_mutable(&pointer_device.events.swipe_end, &event);
}

void touchpad_pinch_begin(int fingers) {
	wlr_pointer_pinch_begin_event event = {.pointer = &pointer_device,
		.time_msec = event_time(), .fingers = static_cast<uint32_t>(fingers)};
	wl_signal_emit_mutable(&pointer_device.events.pinch_begin, &event);
}

void touchpad_pinch_update(int fingers, double scale) {
	wlr_pointer_pinch_update_event event = {.pointer = &pointer_device,
		.time_msec = event_time(), .fingers = static_cast<uint32_t>(fingers), .dx = 0,
		.dy = 0, .scale = scale, .rotation = 0};
	wl_signal_emit_mutable(&pointer_device.events.pinch_update, &event);
}

void touchpad_pinch_end() {
	wlr_pointer_pinch_end_event event = {.pointer = &pointer_device,
		.time_msec = event_time(), .cancelled = false};
	wl_signal_emit_mutable(&pointer_device.events.pinch_end, &event);
}

// --------------------------------------------------------- touchscreen

void touch_frame() {
	wl_signal_emit_mutable(&touch_device.events.frame, nullptr);
}

void finger_down(int32_t id, double x, double y) {
	wlr_touch_down_event event = {.touch = &touch_device, .time_msec = event_time(),
		.touch_id = id, .x = fraction_x(x), .y = fraction_y(y)};
	wl_signal_emit_mutable(&touch_device.events.down, &event);
	touch_frame();
}

void finger_move(int32_t id, double x, double y, bool frame) {
	wlr_touch_motion_event event = {.touch = &touch_device, .time_msec = event_time(),
		.touch_id = id, .x = fraction_x(x), .y = fraction_y(y)};
	wl_signal_emit_mutable(&touch_device.events.motion, &event);
	if (frame) {
		touch_frame();
	}
}

void finger_up(int32_t id) {
	wlr_touch_up_event event = {.touch = &touch_device, .time_msec = event_time(),
		.touch_id = id};
	wl_signal_emit_mutable(&touch_device.events.up, &event);
	touch_frame();
}

void fingers_drag(std::span<const Point> from, double dx, double dy, int steps) {
	for (std::size_t i = 0; i < from.size(); i++) {
		finger_down(static_cast<int32_t>(10 + i), from[i].x, from[i].y);
	}
	for (int s = 1; s <= steps; s++) {
		for (std::size_t i = 0; i < from.size(); i++) {
			finger_move(static_cast<int32_t>(10 + i), from[i].x + dx * s / steps,
				from[i].y + dy * s / steps, false);
		}
		touch_frame();
	}
	for (std::size_t i = 0; i < from.size(); i++) {
		finger_up(static_cast<int32_t>(10 + i));
	}
}

// ----------------------------------------------------------------- pen

void pen_proximity(double x, double y, bool in) {
	wlr_tablet_tool_proximity_event event = {.tablet = &tablet_device, .tool = &pen,
		.time_msec = event_time(), .x = fraction_x(x), .y = fraction_y(y),
		.state = in ? WLR_TABLET_TOOL_PROXIMITY_IN : WLR_TABLET_TOOL_PROXIMITY_OUT};
	wl_signal_emit_mutable(&tablet_device.events.proximity, &event);
}

void pen_move(double x, double y, std::optional<double> pressure, std::optional<Point> tilt) {
	wlr_tablet_tool_axis_event event = {.tablet = &tablet_device, .tool = &pen,
		.time_msec = event_time(),
		.updated_axes = WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y,
		.x = fraction_x(x), .y = fraction_y(y)};
	if (pressure.has_value()) {
		event.updated_axes |= WLR_TABLET_TOOL_AXIS_PRESSURE;
		event.pressure = *pressure;
	}
	if (tilt.has_value()) {
		event.updated_axes |= WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y;
		event.tilt_x = tilt->x;
		event.tilt_y = tilt->y;
	}
	wl_signal_emit_mutable(&tablet_device.events.axis, &event);
}

void pen_tip(double x, double y, bool down) {
	wlr_tablet_tool_tip_event event = {.tablet = &tablet_device, .tool = &pen,
		.time_msec = event_time(), .x = fraction_x(x), .y = fraction_y(y),
		.state = down ? WLR_TABLET_TOOL_TIP_DOWN : WLR_TABLET_TOOL_TIP_UP};
	wl_signal_emit_mutable(&tablet_device.events.tip, &event);
}

void pen_button(uint32_t code, bool pressed) {
	wlr_tablet_tool_button_event event = {.tablet = &tablet_device, .tool = &pen,
		.time_msec = event_time(), .button = code,
		.state = pressed ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED};
	wl_signal_emit_mutable(&tablet_device.events.button, &event);
}

} // namespace harness
