// input.cpp: seats, devices, pointer/keyboard handling. Gesture
// recognition lives in gesture.cpp, pointer grabs in grab.cpp, key
// bindings in bindings.cpp.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <linux/input-event-codes.h>
#include "canvas/items.h"
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
namespace {

constexpr uint32_t binding_mods = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;

// ---- small helpers ----

void notify_activity(Server &server) {
	wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
}

bool box_contains(const wlr_box &box, double x, double y) {
	return x >= box.x && x < box.x + box.width &&
		y >= box.y && y < box.y + box.height;
}

uint32_t seat_modifiers(Server &server) {
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard == nullptr) {
		return 0;
	}
	return wlr_keyboard_get_modifiers(keyboard) & binding_mods;
}

// Hover bookkeeping without extra server state: clear every view's hover
// except the one under the pointer. Returns true when anything changed.
bool set_view_hover(Server &server, View *hover, DecoPart part) {
	bool changed = false;
	for (View *view : server.views) {
		DecoPart want = view == hover ? part : DecoPart::none;
		if (view->deco.hover != want) {
			view->deco.hover = want;
			view->damage();
			changed = true;
		}
	}
	return changed;
}

bool is_logo_key(std::span<const xkb_keysym_t> syms) {
	for (xkb_keysym_t sym : syms) {
		if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
			return true;
		}
	}
	return false;
}

// ---- pointer hit testing ----

struct PointerHit {
	wlr_surface *surface = nullptr; // client surface under the pointer
	View *view = nullptr;            // view frame under the pointer (or null)
	LayerSurface *layer = nullptr;   // layer surface's owner (or null)
	Item *item = nullptr;            // canvas item under the pointer (or null)
	double sx = 0, sy = 0;           // surface-local coordinates
	DecoPart part = DecoPart::none;  // decoration part when surface == null
	uint32_t edges = 0;              // resize edges for DecoPart::resize
};

// The shell surface (top or overlay layer: the bar, panels, the toolbar)
// under the cursor, if any.
std::optional<LayerHit> shell_surface_at(Server &server, Output &output) {
	double ox = server.cursor->x - output.lx;
	double oy = server.cursor->y - output.ly;
	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			l >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; l--) {
		if (std::optional<LayerHit> hit = output.layer_surface_at(l, ox, oy)) {
			return hit;
		}
	}
	return std::nullopt;
}

// A click on a shell surface that takes the keyboard gives it the keyboard,
// recorded as the holder so the keyboard goes back to the window when the
// surface goes; an exclusive holder elsewhere keeps it.
void focus_layer_on_click(Server &server, LayerSurface &layer, wlr_surface *surface) {
	if (!layer.wants_keyboard()) {
		return;
	}
	LayerSurface *holder = server.focused_layer;
	if (holder == nullptr || holder == &layer || !holder->holds_keyboard_exclusively()) {
		server.focused_layer = &layer;
		server.focus_keyboard(surface);
	}
}

PointerHit pointer_hit_test(Server &server, Output &output) {
	PointerHit hit;
	double ox = server.cursor->x - output.lx;
	double oy = server.cursor->y - output.ly;

	if (std::optional<LayerHit> shell = shell_surface_at(server, output)) {
		hit.surface = shell->surface;
		hit.layer = shell->layer;
		hit.sx = shell->sx;
		hit.sy = shell->sy;
		return hit;
	}

	Point p = output.local_to_canvas(ox, oy);

	// Items sit above the windows, below the top and overlay layers, in
	// every view: they win over whatever window is under them.
	if (Item *item = server.items.at(p.x, p.y)) {
		hit.item = item;
		return hit;
	}
	std::optional<ViewHit> view_hit =
		server.view_at(p.x, p.y, output.camera.drawn().zoom);
	if (view_hit.has_value()) {
		hit.view = view_hit->view;
		hit.surface = view_hit->surface;
		hit.sx = view_hit->sx;
		hit.sy = view_hit->sy;
		hit.part = view_hit->part;
		hit.edges = view_hit->edges;
		return hit;
	}

	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
			l >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; l--) {
		std::optional<LayerHit> layer_hit = output.layer_surface_at(l, ox, oy);
		if (layer_hit.has_value()) {
			hit.surface = layer_hit->surface;
			hit.layer = layer_hit->layer;
			hit.sx = layer_hit->sx;
			hit.sy = layer_hit->sy;
			return hit;
		}
	}
	return hit;
}

uint32_t quadrant_edges(View &view, double cx, double cy) {
	wlr_box frame = view.frame_box();
	uint32_t edges = 0;
	edges |= (cx - frame.x) * 2 < frame.width ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
	edges |= (cy - frame.y) * 2 < frame.height ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
	return edges;
}

// What an overview press landed on, following the render order: shelf
// item, tab buttons, tab body, canvas item, window, desktop interior,
// empty.
PressTarget overview_press_target(Server &server, Output &output, double ox,
		double oy, double cx, double cy, double zoom) {
	Shelf shelf = overview_shelf_layout(output);
	for (const ShelfItem &item : shelf) {
		if (box_contains(item.box, ox, oy)) {
			return PressOnShelf{.view = item.view};
		}
	}

	for (const Desktop &desktop : server.canvas.desktops.all()) {
		std::optional<TabLayout> tab = server.painter.overview_tab_layout(output, desktop);
		if (!tab.has_value()) {
			continue;
		}
		if (box_contains(tab->tile_button, ox, oy)) {
			return PressOnTile{.desktop = desktop.id()};
		}
		if (box_contains(tab->abandon_button, ox, oy)) {
			return PressOnAbandon{.desktop = desktop.id()};
		}
		if (box_contains(tab->tab, ox, oy)) {
			return PressOnDesktop{.desktop = desktop.id()};
		}
	}

	// Canvas items sit above windows, below the shelf and the tabs.
	if (Item *item = server.items.at(cx, cy)) {
		return PressOnItem{.item = item};
	}

	std::optional<ViewHit> hit = server.view_at(cx, cy, zoom);
	if (hit.has_value()) {
		return PressOnView{.view = hit->view};
	}

	if (Desktop *desktop = server.canvas.desktops.at(cx, cy)) {
		return PressOnDesktop{.desktop = desktop->id()};
	}
	return PressOnEmpty{};
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
	server_.input.update_capabilities();
	destroy_owned(keyboards, this);
}

Input::Input(Server &server)
	: grab(server), server_(server) {
}

Input::~Input() = default;

void Input::update_capabilities() {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!keyboards.empty()) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server_.seat, caps);
}

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
	if (libinput_device_config_scroll_has_natural_scroll(dev)) {
		libinput_device_config_scroll_set_natural_scroll_enabled(dev,
			config::natural_scroll);
	}
	if (libinput_device_config_dwt_is_available(dev)) {
		libinput_device_config_dwt_set_enabled(dev, config::disable_while_typing
			? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
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
	default:
		break;
	}
	update_capabilities();
}

void Input::handle_new_virtual_keyboard(wlr_virtual_keyboard_v1 *keyboard) {
	// A virtual keyboard (wtype) may carry its own keymap; keep it then.
	new_keyboard(&keyboard->keyboard.base, keyboard->has_keymap);
	update_capabilities();
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
	uint32_t all_mods = wlr_keyboard_get_modifiers(wlr);
	uint32_t mods = all_mods & binding_mods;
	std::span<const xkb_keysym_t> sym_list(syms, static_cast<std::size_t>(nsyms));
	bool logo = is_logo_key(sym_list);

	bool handled = false;
	bool toggle_launcher = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		// A logo key pressed on its own may become a tap (see release).
		super_tap_ = logo && wlr->num_keycodes == 1;
		Output *output = server.output_under_cursor();
		if (server.launcher.is_open()) {
			// The launcher owns the keyboard while open.
			if (nsyms > 0 && !logo) {
				if (xkb_keysym_to_lower(syms[0]) == XKB_KEY_space &&
						(mods & WLR_MODIFIER_LOGO)) {
					server.launcher.close();
				} else {
					server.launcher.key(syms[0],
						xkb_state_key_get_utf32(wlr->xkb_state, keycode), all_mods);
				}
			}
			handled = true;
		} else if (output != nullptr && output->overview.active()) {
			// A shell surface holding the keyboard (a panel, the note editor)
			// takes the keys; Super chords stay the overview's, so Super+Tab
			// still leaves it.
			LayerSurface *holder = server.focused_layer;
			bool shell_types = holder != nullptr && holder->mapped
				&& (mods & WLR_MODIFIER_LOGO) == 0;
			if (!shell_types) {
				for (xkb_keysym_t sym : sym_list) {
					if (server.bindings.handle_overview(sym, mods, *output)) {
						break;
					}
				}
				handled = true; // the overview owns every other key
			}
		} else {
			for (xkb_keysym_t sym : sym_list) {
				if (server.bindings.handle(sym, mods, output)) {
					handled = true;
					break;
				}
			}
		}
		if (handled && event->keycode < 256) {
			key_consumed_[event->keycode] = true;
		}
	} else {
		// Logo pressed and released with nothing in between, like Windows.
		toggle_launcher = super_tap_ && logo;
		super_tap_ = false;
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
	if (toggle_launcher) {
		server.launcher.toggle(server.output_under_cursor());
	}
}

// ---- cursor image and position ----

void Input::set_cursor_image(const char *name) {
	if (name != nullptr && cursor_image != nullptr &&
			std::strcmp(cursor_image, name) == 0) {
		return;
	}
	cursor_image = name;
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

void Input::forget(const View &view) {
	if (last_title_view == &view) {
		last_title_view = nullptr;
	}
	grab.forget(view);
}

// ---- pointer motion ----

void Input::process_cursor_motion(uint32_t time) {
	Server &server = server_;
	if (grab.active()) {
		grab.motion();
		return;
	}

	Output *output = server.output_at(server.cursor->x, server.cursor->y);
	if (output == nullptr) {
		wlr_seat_pointer_notify_clear_focus(server.seat);
		set_cursor_image("default");
		return;
	}
	if (server.launcher.pointer_motion(*output, server.cursor->x - output->lx,
			server.cursor->y - output->ly)) {
		wlr_seat_pointer_notify_clear_focus(server.seat);
		set_view_hover(server, nullptr, DecoPart::none);
		set_cursor_image("default");
		return;
	}

	if (!output->interactive()) {
		// Shell surfaces (the bar, panels, the toolbar) keep the pointer in
		// the overview and during transitions.
		if (std::optional<LayerHit> shell = shell_surface_at(server, *output)) {
			if (set_view_hover(server, nullptr, DecoPart::none)) {
				output->damage();
			}
			wlr_seat_pointer_notify_enter(server.seat, shell->surface, shell->sx, shell->sy);
			wlr_seat_pointer_notify_motion(server.seat, time, shell->sx, shell->sy);
			return;
		}
		// Overview / animating: the compositor owns the pointer. Keep the
		// title bar hover state of the view under the cursor alive so the
		// buttons light up, but no client sees any event.
		wlr_seat_pointer_notify_clear_focus(server.seat);
		Camera cam = output->camera.drawn();
		Point p = output->local_to_canvas(server.cursor->x - output->lx,
			server.cursor->y - output->ly);
		std::optional<ViewHit> hit = server.view_at(p.x, p.y, cam.zoom);
		DecoPart part = hit ? hit->part : DecoPart::none;
		View *view = hit ? hit->view : nullptr;
		wlr_surface *surface = hit ? hit->surface : nullptr;
		if (set_view_hover(server, surface != nullptr ? nullptr : view, part)) {
			output->damage();
		}
		set_cursor_image("default");
		return;
	}

	PointerHit hit = pointer_hit_test(server, *output);
	if (hit.surface != nullptr) {
		set_view_hover(server, nullptr, DecoPart::none);
		wlr_seat_pointer_notify_enter(server.seat, hit.surface, hit.sx, hit.sy);
		wlr_seat_pointer_notify_motion(server.seat, time, hit.sx, hit.sy);
		// The client owns the cursor image while it has pointer focus.
	} else {
		wlr_seat_pointer_notify_clear_focus(server.seat);
		if (hit.view != nullptr) {
			set_view_hover(server, hit.view, hit.part);
			set_cursor_image(hit.part == DecoPart::resize
				? wlr_xcursor_get_resize_name(static_cast<wlr_edges>(hit.edges))
					: "default");
		} else {
			set_view_hover(server, nullptr, DecoPart::none);
			set_cursor_image("default");
		}
	}
}

void Input::rebase_cursor() {
	if (!grab.active()) {
		process_cursor_motion(static_cast<uint32_t>(now_ms()));
	}
}

// ---- overview pointer interaction ----

void Input::overview_press(Output &output, uint32_t time, uint32_t button) {
	Camera cam = output.camera.drawn();
	double ox = server_.cursor->x - output.lx;
	double oy = server_.cursor->y - output.ly;
	Point p = output.local_to_canvas(ox, oy);

	if (button == BTN_RIGHT) {
		// Right click on the empty canvas or a desktop's interior (not
		// over a window) creates a desktop there.
		if (!server_.view_at(p.x, p.y, cam.zoom).has_value()) {
			output.overview.new_desktop(p.x, p.y);
		}
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}

	PressTarget target =
		overview_press_target(server_, output, ox, oy, p.x, p.y, cam.zoom);
	if (const auto *on_item = std::get_if<PressOnItem>(&target)) {
		// A press on an item drags it (or activates it) like in the
		// normal view.
		item_press(time, *on_item->item, output);
		return;
	}
	grab.begin_overview_press(output, target);
}

void Input::item_press(uint32_t time, Item &item, Output &output) {
	if (last_item_id == item.id && time - last_item_click <= config::double_click_ms) {
		last_item_id.clear(); // a third press starts a fresh pair
		server_.ipc.emit_item_activated(item, output);
		return;
	}
	last_item_click = time;
	last_item_id = item.id;
	grab.begin_item(item, output);
}

// ---- pointer buttons ----

void Input::forward_press(uint32_t time, uint32_t button) {
	pressed_buttons_ |= 1u << (button & 31);
	wlr_seat_pointer_notify_button(server_.seat, time, button,
		WL_POINTER_BUTTON_STATE_PRESSED);
}

void Input::cursor_button_press(uint32_t time, uint32_t button) {
	Server &server = server_;
	super_tap_ = false;
	if (grab.active()) {
		return; // extra buttons during a compositor grab are swallowed
	}
	Output *output = server.output_at(server.cursor->x, server.cursor->y);
	if (output == nullptr) {
		return;
	}
	if (server.launcher.pointer_button(*output, server.cursor->x - output->lx,
			server.cursor->y - output->ly, button, true)) {
		return;
	}
	if (!output->interactive()) {
		if (std::optional<LayerHit> shell = shell_surface_at(server, *output)) {
			// Pointer focus is unset if the cursor has not moved since the
			// overview opened.
			wlr_seat_pointer_notify_enter(server.seat, shell->surface, shell->sx, shell->sy);
			focus_layer_on_click(server, *shell->layer, shell->surface);
			forward_press(time, button);
			return;
		}
		overview_press(*output, time, button);
		return;
	}

	bool super = seat_modifiers(server) & WLR_MODIFIER_LOGO;
	PointerHit hit = pointer_hit_test(server, *output);

	if (hit.item != nullptr) {
		// Items take precedence over the windows under them; a press
		// focuses no window and takes no keyboard.
		if (button == BTN_LEFT) {
			item_press(time, *hit.item, *output);
		}
		return;
	}

	if (hit.view != nullptr && super && button == BTN_LEFT) {
		server.focus_view(hit.view);
		grab.begin_move(*hit.view, *output);
		return;
	}
	if (hit.view != nullptr && super && button == BTN_RIGHT) {
		server.focus_view(hit.view);
		Point p = output->local_to_canvas(server.cursor->x - output->lx,
			server.cursor->y - output->ly);
		grab.begin_resize(*hit.view, *output, quadrant_edges(*hit.view, p.x, p.y));
		return;
	}

	if (hit.surface != nullptr) {
		if (hit.view != nullptr) {
			server.focus_view(hit.view);
		} else if (hit.layer != nullptr) {
			focus_layer_on_click(server, *hit.layer, hit.surface);
		}
		forward_press(time, button);
		return;
	}

	if (hit.view != nullptr) {
		// Somewhere on the server-side frame.
		server.focus_view(hit.view);
		switch (hit.part) {
		case DecoPart::title:
			if (button == BTN_LEFT) {
				if (last_title_view == hit.view &&
						time - last_title_click <= config::double_click_ms) {
					last_title_view = nullptr;
					hit.view->set_maximized(!hit.view->maximized);
				} else {
					last_title_click = time;
					last_title_view = hit.view;
					grab.begin_move(*hit.view, *output);
				}
			}
			return;
		case DecoPart::minimize:
		case DecoPart::maximize:
		case DecoPart::close:
			if (button == BTN_LEFT) {
				grab.begin_title_button(*hit.view, *output, hit.part);
			}
			return;
		case DecoPart::resize:
			if (button == BTN_LEFT || button == BTN_RIGHT) {
				grab.begin_resize(*hit.view, *output, hit.edges);
			}
			return;
		default:
			return;
		}
	}

	if (button == BTN_LEFT) {
		grab.begin_pan(*output);
	}
}

void Input::cursor_button_release(uint32_t time, uint32_t button) {
	if (!grab.active()) {
		uint32_t bit = 1u << (button & 31);
		if (pressed_buttons_ & bit) {
			pressed_buttons_ &= ~bit;
			wlr_seat_pointer_notify_button(server_.seat, time, button,
				WL_POINTER_BUTTON_STATE_RELEASED);
		} else {
			Output *output =
				server_.output_at(server_.cursor->x, server_.cursor->y);
			if (output != nullptr) {
				server_.launcher.pointer_button(*output,
					server_.cursor->x - output->lx, server_.cursor->y - output->ly,
					button, false);
			}
		}
		return;
	}
	grab.release();
}

// ---- cursor event handlers ----

void Input::handle_cursor_motion(wlr_pointer_motion_event *event) {
	notify_activity(server_);
	wlr_cursor_move(server_.cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(event->time_msec);
}

void Input::handle_cursor_motion_absolute(
		wlr_pointer_motion_absolute_event *event) {
	notify_activity(server_);
	wlr_cursor_warp_absolute(server_.cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(event->time_msec);
}

void Input::handle_cursor_button(wlr_pointer_button_event *event) {
	notify_activity(server_);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		cursor_button_press(event->time_msec, event->button);
	} else {
		cursor_button_release(event->time_msec, event->button);
	}
}

void Input::handle_cursor_axis(wlr_pointer_axis_event *event) {
	Server &server = server_;
	notify_activity(server);
	super_tap_ = false;
	if (grab.active()) {
		return;
	}
	Output *output = server.output_at(server.cursor->x, server.cursor->y);
	if (output == nullptr) {
		return;
	}
	if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
			server.launcher.pointer_axis(*output, server.cursor->x - output->lx,
				server.cursor->y - output->ly, event->delta)) {
		return;
	}
	// A shell surface under the pointer scrolls, in the overview too.
	if (output->overview.active() && !shell_surface_at(server, *output)) {
		if (seat_modifiers(server) & WLR_MODIFIER_CTRL) {
			output->overview.zoom_by(std::exp(-event->delta / 200.0),
				server.cursor->x - output->lx, server.cursor->y - output->ly);
		} else {
			double delta = event->delta * config::scroll_pan_speed;
			Camera cam = output->camera.drawn();
			if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
				cam.cy += delta / cam.zoom;
			} else {
				cam.cx += delta / cam.zoom;
			}
			output->camera.jump(cam);
		}
		return;
	}
	if (server.seat->pointer_state.focused_surface != nullptr) {
		wlr_seat_pointer_notify_axis(server.seat, event->time_msec,
			event->orientation, event->delta, event->delta_discrete,
			event->source, event->relative_direction);
		return;
	}
	// Scrolling over the empty canvas pans; libinput already applied the
	// natural-scroll direction.
	if (output->interactive()) {
		// A scroll pans the canvas at the camera's zoom: the screen moves
		// by the scrolled distance whatever the zoom.
		double delta = event->delta * config::scroll_pan_speed
			/ output->camera.current().zoom;
		if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			output->pan(0, delta);
		} else {
			output->pan(delta, 0);
		}
	}
}

void Input::handle_cursor_frame() {
	wlr_seat_pointer_notify_frame(server_.seat);
}

// ---- touchpad gestures (recognition is delegated to gesture.cpp) ----

void Input::handle_swipe_begin(wlr_pointer_swipe_begin_event *event) {
	notify_activity(server_);
	super_tap_ = false;
	server_.gestures.swipe_begin(event->time_msec, event->fingers);
}

void Input::handle_swipe_update(wlr_pointer_swipe_update_event *event) {
	notify_activity(server_);
	server_.gestures.swipe_update(event->time_msec, event->dx, event->dy);
}

void Input::handle_swipe_end(wlr_pointer_swipe_end_event *event) {
	notify_activity(server_);
	server_.gestures.swipe_end(event->time_msec, event->cancelled);
}

void Input::handle_pinch_begin(wlr_pointer_pinch_begin_event *event) {
	notify_activity(server_);
	super_tap_ = false;
	server_.gestures.pinch_begin(event->time_msec, event->fingers);
}

void Input::handle_pinch_update(wlr_pointer_pinch_update_event *event) {
	notify_activity(server_);
	server_.gestures.pinch_update(event->time_msec, event->dx, event->dy,
		event->scale, event->rotation);
}

void Input::handle_pinch_end(wlr_pointer_pinch_end_event *event) {
	notify_activity(server_);
	server_.gestures.pinch_end(event->time_msec, event->cancelled);
}

void Input::handle_hold_begin(wlr_pointer_hold_begin_event *event) {
	notify_activity(server_);
	server_.gestures.hold_begin(event->time_msec, event->fingers);
}

void Input::handle_hold_end(wlr_pointer_hold_end_event *event) {
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
		cursor_image = nullptr;
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

void Input::handle_request_start_drag(
		wlr_seat_request_start_drag_event *event) {
	if (!wlr_seat_validate_pointer_grab_serial(server_.seat, event->origin,
			event->serial)) {
		if (event->drag->source != nullptr) {
			wlr_data_source_destroy(event->drag->source);
		}
		return;
	}
	// Start the drag itself; re-requesting it would re-emit this event forever.
	wlr_seat_start_pointer_drag(server_.seat, event->drag, event->serial);
}

// ---- drag and drop ----

void Input::handle_drag_icon_commit(wlr_surface *surface) {
	drag_icon_dx += surface->current.dx;
	drag_icon_dy += surface->current.dy;
	server_.damage_all();
}

void Input::handle_drag_icon_destroy() {
	drag_icon_commit_.disconnect();
	drag_icon_destroy_.disconnect();
	drag_icon = nullptr;
	drag_icon_dx = 0;
	drag_icon_dy = 0;
	server_.damage_all();
}

void Input::handle_start_drag(wlr_drag *drag) {
	drag_icon_dx = 0;
	drag_icon_dy = 0;
	drag_icon = drag->icon;
	if (drag->icon == nullptr) {
		return;
	}
	drag_icon_commit_.connect<&Input::handle_drag_icon_commit>(
		drag->icon->surface->events.commit, this);
	drag_icon_destroy_.connect<&Input::handle_drag_icon_destroy>(
		drag->icon->events.destroy, this);
	server_.damage_all();
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
	request_start_drag_.connect<&Input::handle_request_start_drag>(
		server.seat->events.request_start_drag, this);
	start_drag_.connect<&Input::handle_start_drag>(
		server.seat->events.start_drag, this);
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
	request_start_drag_.disconnect();
	start_drag_.disconnect();
	if (drag_icon != nullptr) {
		drag_icon_commit_.disconnect();
		drag_icon_destroy_.disconnect();
		drag_icon = nullptr;
		drag_icon_dx = 0;
		drag_icon_dy = 0;
	}
}
