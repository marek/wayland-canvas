// input.cpp: the seat and its devices, and the dispatch of their events.
// The pointer's routing lives in pointer_router.cpp, gesture recognition in
// gesture.cpp, pointer grabs in grab.cpp, key bindings in bindings.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/process.h"
#include "input/ui_layer.h"

namespace {

constexpr uint32_t binding_mods = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;

// ---- small helpers ----

void notify_activity(Server &server) {
	wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
	server.lock.notice_activity();
}

bool is_logo_key(std::span<const xkb_keysym_t> syms) {
	for (xkb_keysym_t sym : syms) {
		if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
			return true;
		}
	}
	return false;
}

// The key's unshifted symbols: Super+Shift+3 is '#' on a US layout, so the
// digit chords are matched through their level-0 symbol as well.
std::span<const xkb_keysym_t> base_syms(wlr_keyboard *wlr, uint32_t keycode) {
	xkb_layout_index_t layout = xkb_state_key_get_layout(wlr->xkb_state, keycode);
	if (layout == XKB_LAYOUT_INVALID) {
		return {};
	}
	const xkb_keysym_t *syms = nullptr;
	int count = xkb_keymap_key_get_syms_by_level(wlr->keymap, keycode, layout, 0, &syms);
	return {syms, static_cast<std::size_t>(count)};
}


} // namespace

// ---- keyboard devices ----

Keyboard::Keyboard(Server &server, wlr_keyboard *wlr, bool has_keymap)
	: wlr(wlr), server_(server) {
	if (!has_keymap) {
		// Null names: xkbcommon applies its defaults, honouring
		// XKB_DEFAULT_RULES/MODEL/LAYOUT/VARIANT/OPTIONS from the environment.
		xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (context != nullptr) {
			xkb_keymap *keymap = xkb_keymap_new_from_names(context, nullptr,
				XKB_KEYMAP_COMPILE_NO_FLAGS);
			if (keymap != nullptr) {
				wlr_keyboard_set_keymap(wlr, keymap);
				xkb_keymap_unref(keymap);
			}
			xkb_context_unref(context);
		}
	}
	wlr_keyboard_set_repeat_info(wlr, config::repeat_rate, config::repeat_delay);
	modifiers_.connect<&Keyboard::handle_modifiers>(wlr->events.modifiers, this);
	key_.connect<&Keyboard::handle_key>(wlr->events.key, this);
	destroy_.connect<&Keyboard::handle_destroy>(wlr->base.events.destroy, this);
}

Keyboard::~Keyboard() = default;

void Keyboard::handle_modifiers() {
	notify_activity(server_);
	wlr_seat_set_keyboard(server_.seat, wlr);
	wlr_seat_keyboard_notify_modifiers(server_.seat, &wlr->modifiers);
}

void Keyboard::handle_key(wlr_keyboard_key_event *event) {
	server_.input.process_key(*this, event);
}

void Keyboard::handle_destroy() {
	auto &keyboards = server_.input.keyboards;
	if (keyboards.size() == 1) {
		wlr_seat_set_keyboard(server_.seat, nullptr);
	} else {
		// The first keyboard left in the list takes the seat.
		Keyboard *next = keyboards.front().get() == this
			? keyboards[1].get() : keyboards.front().get();
		wlr_seat_set_keyboard(server_.seat, next->wlr);
	}
	destroy_owned(keyboards, this);
}

PointerDevice::PointerDevice(Input &input, wlr_input_device *device,
		libinput_device *handle)
		: dev(handle), input_(input) {
	destroy_.connect<&PointerDevice::handle_destroy>(device->events.destroy, this);
}

void PointerDevice::handle_destroy() {
	input_.x_gate.run_waiting(); // a waiting event names this device
	destroy_owned(input_.pointers, this);
}

Input::Input(Server &server)
	: grab(server), x_gate(server), super_tap(server),
		shortcut_capture(server, super_tap), router(server, *this), dnd(server),
		touch(server, *this), tablets(server, *this), server_(server) {
}

Input::~Input() = default;

void Input::new_keyboard(wlr_input_device *device, bool has_keymap) {
	wlr_keyboard *wlr = wlr_keyboard_from_input_device(device);
	auto keyboard = std::make_unique<Keyboard>(server_, wlr, has_keymap);
	wlr_seat_set_keyboard(server_.seat, wlr);
	keyboards.insert(keyboards.begin(), std::move(keyboard));
}

void Input::new_pointer(wlr_input_device *device) {
	wlr_cursor_attach_input_device(server_.cursor, device);
	if (!wlr_input_device_is_libinput(device)) {
		return;
	}
	libinput_device *dev = wlr_libinput_get_device_handle(device);
	if (dev == nullptr) {
		return;
	}
	if (libinput_device_config_tap_get_finger_count(dev) > 0) {
		libinput_device_config_tap_set_enabled(dev, config::tap_to_click
			? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	}
	libinput_config_click_method click = config::clickfinger
		? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER : LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
	if ((libinput_device_config_click_get_methods(dev) & click) != 0) {
		libinput_device_config_click_set_method(dev, click);
	}
	if (libinput_device_config_dwt_is_available(dev)) {
		libinput_device_config_dwt_set_enabled(dev, config::disable_while_typing
			? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	}
	apply_pointer_settings(dev);
	pointers.push_back(std::make_unique<PointerDevice>(*this, device, dev));
}

// The scroll direction canvas.conf chooses for apps, the mouse wheel's and
// the touchpad's apart: natural unless set traditional
// (docs/intents/input/gestures.md). A touchpad is the device that taps.
void Input::apply_pointer_settings(libinput_device *dev) {
	if (libinput_device_config_scroll_has_natural_scroll(dev)) {
		bool touchpad = libinput_device_config_tap_get_finger_count(dev) > 0;
		bool traditional = touchpad ? server_.conf.resolved.touchpad_traditional
			: server_.conf.resolved.mouse_traditional;
		libinput_device_config_scroll_set_natural_scroll_enabled(dev, !traditional);
	}
}

void Input::settings_changed() {
	for (const std::unique_ptr<PointerDevice> &pointer : pointers) {
		apply_pointer_settings(pointer->dev);
	}
}

void Input::handle_new_input(wlr_input_device *device) {
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(device, false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		new_pointer(device);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		touch.new_device(device);
		break;
	case WLR_INPUT_DEVICE_TABLET:
		tablets.new_device(device);
		break;
	default:
		break;
	}
}

void Input::handle_new_virtual_keyboard(wlr_virtual_keyboard_v1 *keyboard) {
	// A virtual keyboard (wtype) may carry its own keymap; keep it then.
	new_keyboard(&keyboard->keyboard.base, keyboard->has_keymap);
}

// ---- the compositor's own UIs ----

UiLayer *Input::layer_for_keys() const {
	for (UiLayer *layer : layers_) {
		if (layer->owns_keyboard()) {
			return layer;
		}
	}
	return nullptr;
}

UiLayer *Input::layer_at(const Output &output, double ox, double oy) const {
	for (UiLayer *layer : layers_) {
		if (layer->takes_pointer_at(output, ox, oy)) {
			return layer;
		}
	}
	return nullptr;
}

bool Input::gestures_blocked() const {
	return std::ranges::any_of(layers_,
		[](const UiLayer *layer) { return layer->blocks_gestures(); });
}

// ---- key events ----

void Input::process_key(Keyboard &keyboard, wlr_keyboard_key_event *event) {
	Server &server = server_;
	wlr_seat *seat = server.seat;
	wlr_keyboard *wlr = keyboard.wlr;
	notify_activity(server);

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms = nullptr;
	int nsyms = xkb_state_key_get_syms(wlr->xkb_state, keycode, &syms);
	std::span<const xkb_keysym_t> produced(syms, static_cast<std::size_t>(nsyms));
	uint32_t mods = wlr_keyboard_get_modifiers(wlr) & binding_mods;
	if (server.lock.locked()) {
		// Only the locker and VT switching; no canvas shortcut, no tap.
		super_tap.disarm();
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			for (xkb_keysym_t sym : produced) {
				if (server.bindings.handle_locked(sym, mods,
						server.output_under_cursor())) {
					return;
				}
			}
		}
		wlr_seat_set_keyboard(seat, wlr);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
			event->state);
		return;
	}
	bool logo = is_logo_key(produced);

	// From begin to end a shortcut capture takes every key: nothing runs,
	// no surface sees one.
	bool handled = shortcut_capture.takes(*event, produced);
	bool tap = false;
	if (handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED
			&& event->keycode < 256) {
		key_consumed_[event->keycode] = true;
	}
	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		// A logo key pressed on its own may become a tap (see release).
		super_tap.key_pressed(logo, wlr->num_keycodes == 1);
		Output *output = server.output_under_cursor();
		KeySyms keys(produced, base_syms(wlr, keycode));
		if (UiLayer *layer = layer_for_keys()) {
			// A layer owns the keyboard: every key is consumed, and nothing
			// it sees reaches a client.
			if (!layer->allows_tap()) {
				super_tap.disarm();
			}
			layer->key({.syms = keys,
				.utf32 = xkb_state_key_get_utf32(wlr->xkb_state, keycode),
				.mods = mods, .logo = logo});
			handled = true;
		} else if (output != nullptr && output->overview.active()) {
			// A shell surface holding the keyboard (a panel, the note editor)
			// takes the keys; Super chords stay the overview's, so Super+Tab
			// still leaves it.
			LayerSurface *holder = server.focused_layer;
			bool shell_types = holder != nullptr && holder->mapped
				&& (mods & WLR_MODIFIER_LOGO) == 0;
			if (!shell_types) {
				keys.any([&](xkb_keysym_t sym) {
					return server.bindings.handle_overview(sym, mods, *output);
				});
				handled = true; // the overview owns every other key
			}
		} else {
			handled = keys.any([&](xkb_keysym_t sym) {
				return server.bindings.handle(sym, mods, output);
			});
		}
		if (handled && event->keycode < 256) {
			key_consumed_[event->keycode] = true;
		}
	} else if (!handled) {
		// Logo pressed and released with nothing in between: the tap runs
		// whichever action holds `Super`.
		tap = super_tap.key_released(logo);
		if (event->keycode < 256 && key_consumed_[event->keycode]) {
			// Release of a press the client never saw.
			key_consumed_[event->keycode] = false;
			handled = true;
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, wlr);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
			event->state);
	}
	if (tap) {
		super_tap.lone_tap();
	}
}

// ---- cursor image and position ----

void Input::set_cursor_image(const char *name) {
	if (name != nullptr && cursor_image_ != nullptr &&
			std::strcmp(cursor_image_, name) == 0) {
		return;
	}
	cursor_image_ = name;
	if (name != nullptr) {
		wlr_cursor_set_xcursor(server_.cursor, server_.xcursor_mgr, name);
	}
}

std::optional<CanvasPos> Input::cursor_canvas_pos() const {
	Output *output = server_.output_under_cursor();
	if (output == nullptr) {
		return std::nullopt;
	}
	Point p = output->local_to_canvas(server_.cursor->x - output->lx,
		server_.cursor->y - output->ly);
	return CanvasPos{.output = output, .cx = p.x, .cy = p.y};
}

Desktop *Input::desktop_under_cursor(const Output &output) const {
	std::optional<CanvasPos> pos = cursor_canvas_pos();
	if (!pos.has_value() || pos->output != &output) {
		return nullptr;
	}
	return server_.canvas.desktops.at(pos->cx, pos->cy);
}

uint32_t Input::held_modifiers() const {
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(server_.seat);
	if (keyboard == nullptr) {
		return 0;
	}
	return wlr_keyboard_get_modifiers(keyboard) & binding_mods;
}

bool Input::super_held() const {
	return (held_modifiers() & WLR_MODIFIER_LOGO) != 0;
}

void Input::forget(const View &view) {
	router.forget(view);
	grab.forget(view);
}

void Input::rebase_cursor() {
	if (!grab.active()) {
		router.motion(static_cast<uint32_t>(now_ms()));
	}
}

// ---- touch and the pen ----

void Input::contact_activity() {
	notify_activity(server_);
	super_tap.other_input();
}

void Input::end_contacts() {
	uint32_t time = static_cast<uint32_t>(now_ms());
	touch.end_all(time);
	tablets.end_all(time);
}

void Input::abandon_interaction() {
	grab.cancel();
	router.end_hold();
	end_contacts();
	shortcut_capture.cancel();
	super_tap.cancel_pending();
}

Output *Input::output_for_device(const char *output_name) {
	Server &server = server_;
	if (output_name != nullptr) {
		for (const std::unique_ptr<Output> &output : server.outputs) {
			if (std::strcmp(output->wlr->name, output_name) == 0) {
				return output.get();
			}
		}
	}
	for (const std::unique_ptr<Output> &output : server.outputs) {
		std::string_view name = output->wlr->name;
		if (name.starts_with("eDP") || name.starts_with("LVDS") || name.starts_with("DSI")) {
			return output.get();
		}
	}
	return server.outputs.empty() ? nullptr : server.outputs.front().get();
}

void Input::update_capabilities() {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
	if (touch.connected()) {
		caps |= WL_SEAT_CAPABILITY_TOUCH;
	}
	wlr_seat_set_capabilities(server_.seat, caps);
}

void Input::outputs_changed() {
	touch.map_devices();
	tablets.map_devices();
}

void Input::output_gone(Output &output) {
	// The layout every touch and pen is placed in changes: they end first,
	// a drag on the compositor's UI as a release would, while all is whole.
	end_contacts();
	grab.output_gone(output);
	for (UiLayer *layer : layers_) {
		layer->output_gone(output);
	}
}

// ---- cursor event handlers ----

void Input::handle_cursor_motion(wlr_pointer_motion_event *event) {
	if (x_gate.waits<&Input::handle_cursor_motion>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	wlr_cursor_move(server_.cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	router.motion(event->time_msec);
}

void Input::handle_cursor_motion_absolute(
		wlr_pointer_motion_absolute_event *event) {
	if (x_gate.waits<&Input::handle_cursor_motion_absolute>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	wlr_cursor_warp_absolute(server_.cursor, &event->pointer->base,
		event->x, event->y);
	router.motion(event->time_msec);
}

void Input::handle_cursor_button(wlr_pointer_button_event *event) {
	if (x_gate.waits<&Input::handle_cursor_button>(this, event,
			event->state == WL_POINTER_BUTTON_STATE_PRESSED
			&& Xwm::is_x11_window(server_.seat->pointer_state.focused_surface))) {
		return;
	}
	notify_activity(server_);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		router.button_press(event->time_msec, event->button);
	} else {
		router.button_release(event->time_msec, event->button);
	}
}

void Input::handle_cursor_axis(wlr_pointer_axis_event *event) {
	// X turns a scroll into button presses, so it waits as a press does.
	if (x_gate.waits<&Input::handle_cursor_axis>(this, event,
			Xwm::is_x11_window(server_.seat->pointer_state.focused_surface))) {
		return;
	}
	notify_activity(server_);
	super_tap.other_input();
	router.axis(*event);
}

void Input::handle_cursor_frame() {
	if (x_gate.waits<&Input::handle_cursor_frame>(this)) {
		return;
	}
	wlr_seat_pointer_notify_frame(server_.seat);
}

// ---- touchpad gestures (recognition is delegated to gesture.cpp) ----

void Input::handle_swipe_begin(wlr_pointer_swipe_begin_event *event) {
	if (x_gate.waits<&Input::handle_swipe_begin>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	super_tap.other_input();
	if (gestures_blocked() || server_.lock.locked()) {
		return; // a layer or the lock owns the pointer
	}
	server_.gestures.swipe_begin(event->time_msec, event->fingers);
}

void Input::handle_swipe_update(wlr_pointer_swipe_update_event *event) {
	if (x_gate.waits<&Input::handle_swipe_update>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	server_.gestures.swipe_update(event->time_msec, event->dx, event->dy);
}

void Input::handle_swipe_end(wlr_pointer_swipe_end_event *event) {
	if (x_gate.waits<&Input::handle_swipe_end>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	server_.gestures.swipe_end(event->time_msec, event->cancelled);
}

void Input::handle_pinch_begin(wlr_pointer_pinch_begin_event *event) {
	if (x_gate.waits<&Input::handle_pinch_begin>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	super_tap.other_input();
	if (gestures_blocked() || server_.lock.locked()) {
		return; // a layer or the lock owns the pointer
	}
	server_.gestures.pinch_begin(event->time_msec, event->fingers);
}

void Input::handle_pinch_update(wlr_pointer_pinch_update_event *event) {
	if (x_gate.waits<&Input::handle_pinch_update>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	server_.gestures.pinch_update(event->time_msec, event->dx, event->dy,
		event->scale, event->rotation);
}

void Input::handle_pinch_end(wlr_pointer_pinch_end_event *event) {
	if (x_gate.waits<&Input::handle_pinch_end>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	server_.gestures.pinch_end(event->time_msec, event->cancelled);
}

void Input::handle_hold_begin(wlr_pointer_hold_begin_event *event) {
	if (x_gate.waits<&Input::handle_hold_begin>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	server_.gestures.hold_begin(event->time_msec, event->fingers);
}

void Input::handle_hold_end(wlr_pointer_hold_end_event *event) {
	if (x_gate.waits<&Input::handle_hold_end>(this, event, false)) {
		return;
	}
	notify_activity(server_);
	server_.gestures.hold_end(event->time_msec, event->cancelled);
}

// ---- seat requests ----

void Input::handle_request_cursor(
		wlr_seat_pointer_request_set_cursor_event *event) {
	wlr_seat_client *focused = server_.seat->pointer_state.focused_client;
	if (focused == event->seat_client) {
		wlr_cursor_set_surface(server_.cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
		// The client owns the image now; our cached name is stale.
		cursor_image_ = nullptr;
	}
}

void Input::handle_request_set_shape(
		wlr_cursor_shape_manager_v1_request_set_shape_event *event) {
	if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
		return;
	}
	if (server_.seat->pointer_state.focused_client == event->seat_client) {
		set_cursor_image(wlr_cursor_shape_v1_name(event->shape));
	}
}

void Input::handle_request_set_selection(
		wlr_seat_request_set_selection_event *event) {
	wlr_seat_set_selection(server_.seat, event->source, event->serial);
}

void Input::handle_request_set_primary_selection(
		wlr_seat_request_set_primary_selection_event *event) {
	wlr_seat_set_primary_selection(server_.seat, event->source, event->serial);
}

// ---- module lifecycle ----

void Input::init() {
	Server &server = server_;
	new_input_.connect<&Input::handle_new_input>(
		server.backend->events.new_input, this);

	// Virtual keyboards (wtype & friends) behave like real ones.
	wlr_virtual_keyboard_manager_v1 *virtual_keyboards =
		wlr_virtual_keyboard_manager_v1_create(server.display);
	new_virtual_keyboard_.connect<&Input::handle_new_virtual_keyboard>(
		virtual_keyboards->events.new_virtual_keyboard, this);

	wlr_cursor_shape_manager_v1 *cursor_shapes =
		wlr_cursor_shape_manager_v1_create(server.display, 1);
	request_cursor_shape_.connect<&Input::handle_request_set_shape>(
		cursor_shapes->events.request_set_shape, this);

	cursor_motion_.connect<&Input::handle_cursor_motion>(
		server.cursor->events.motion, this);
	cursor_motion_absolute_.connect<&Input::handle_cursor_motion_absolute>(
		server.cursor->events.motion_absolute, this);
	cursor_button_.connect<&Input::handle_cursor_button>(
		server.cursor->events.button, this);
	cursor_axis_.connect<&Input::handle_cursor_axis>(
		server.cursor->events.axis, this);
	cursor_frame_.connect<&Input::handle_cursor_frame>(
		server.cursor->events.frame, this);

	swipe_begin_.connect<&Input::handle_swipe_begin>(
		server.cursor->events.swipe_begin, this);
	swipe_update_.connect<&Input::handle_swipe_update>(
		server.cursor->events.swipe_update, this);
	swipe_end_.connect<&Input::handle_swipe_end>(
		server.cursor->events.swipe_end, this);
	pinch_begin_.connect<&Input::handle_pinch_begin>(
		server.cursor->events.pinch_begin, this);
	pinch_update_.connect<&Input::handle_pinch_update>(
		server.cursor->events.pinch_update, this);
	pinch_end_.connect<&Input::handle_pinch_end>(
		server.cursor->events.pinch_end, this);
	hold_begin_.connect<&Input::handle_hold_begin>(
		server.cursor->events.hold_begin, this);
	hold_end_.connect<&Input::handle_hold_end>(
		server.cursor->events.hold_end, this);

	request_cursor_.connect<&Input::handle_request_cursor>(
		server.seat->events.request_set_cursor, this);
	request_set_selection_.connect<&Input::handle_request_set_selection>(
		server.seat->events.request_set_selection, this);
	request_set_primary_selection_.
		connect<&Input::handle_request_set_primary_selection>(
			server.seat->events.request_set_primary_selection, this);

	dnd.init();
	touch.init();
	tablets.init();
}

void Input::finish() {
	new_input_.disconnect();
	new_virtual_keyboard_.disconnect();
	request_cursor_shape_.disconnect();
	cursor_motion_.disconnect();
	cursor_motion_absolute_.disconnect();
	cursor_button_.disconnect();
	cursor_axis_.disconnect();
	cursor_frame_.disconnect();
	swipe_begin_.disconnect();
	swipe_update_.disconnect();
	swipe_end_.disconnect();
	pinch_begin_.disconnect();
	pinch_update_.disconnect();
	pinch_end_.disconnect();
	hold_begin_.disconnect();
	hold_end_.disconnect();
	request_cursor_.disconnect();
	request_set_selection_.disconnect();
	request_set_primary_selection_.disconnect();
	super_tap.finish();
	shortcut_capture.finish();
	x_gate.finish();
	touch.finish();
	tablets.finish();
	dnd.finish();
}
