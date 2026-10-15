// input.cpp: seats, devices, pointer/keyboard handling, compositor grabs
// and key bindings. Gesture recognition lives in gesture.cpp; the actions
// it and this file trigger live in canvas.cpp/view.cpp.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <variant>
#include <vector>
#include <linux/input-event-codes.h>
#include "config.h"
#include "output.h"
#include "server.h"
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

// ---- key bindings ----

// A direction argument: -1, 0 or 1 per axis.
struct Dir {
	int dx = 0, dy = 0;
};

// What a binding passes to its action: a desktop id, a command line or a
// direction.
using KeyArg = std::variant<int, const char *, Dir>;

// Scoped action: called with the server and the output under the cursor
// (possibly null).
using Action = void (*)(Server &, Output *, KeyArg);

struct Keybind {
	uint32_t mods = 0;
	xkb_keysym_t keysym = 0;
	Action fn = nullptr;
	KeyArg arg = 0;
};

void act_spawn(Server &, Output *, KeyArg arg) {
	spawn(std::get<const char *>(arg));
}

void act_launcher(Server &server, Output *output, KeyArg) {
	server.launcher.toggle(output);
}

void act_close(Server &server, Output *, KeyArg) {
	if (server.focused_view != nullptr) {
		server.focused_view->close();
	}
}

void act_minimize(Server &server, Output *, KeyArg) {
	if (server.focused_view != nullptr && server.focused_view->mapped) {
		server.focused_view->set_minimized(true);
	}
}

void act_restore_minimized(Server &server, Output *, KeyArg) {
	View *view = server.last_minimized_view();
	if (view != nullptr) {
		view->activate();
	}
}

void act_maximize(Server &server, Output *, KeyArg) {
	View *view = server.focused_view;
	if (view != nullptr) {
		view->set_maximized(!view->maximized);
	}
}

void act_fullscreen(Server &server, Output *, KeyArg) {
	View *view = server.focused_view;
	if (view != nullptr) {
		view->set_fullscreen(!view->fullscreen);
	}
}

void act_overview(Server &, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	if (output->mode == Mode::overview) {
		output->overview_exit();
	} else {
		output->overview_enter();
	}
}

void act_goto_dir(Server &, Output *output, KeyArg arg) {
	Dir dir = std::get<Dir>(arg);
	if (output != nullptr) {
		output->goto_direction(dir.dx, dir.dy);
	}
}

// Move the focused window onto the nearest desktop in the given direction
// (or by one screen when there is none) and follow it.
void act_move_dir(Server &server, Output *output, KeyArg arg) {
	Dir dir = std::get<Dir>(arg);
	View *view = server.focused_view;
	if (view == nullptr || !view->mapped || output == nullptr) {
		return;
	}
	Desktop *cur = server.canvas.desktop_of(*view);
	double ox, oy;
	if (cur != nullptr) {
		Point centre = cur->centre();
		ox = centre.x;
		oy = centre.y;
	} else {
		Rect vp = output->viewport();
		ox = vp.cx();
		oy = vp.cy();
	}

	Desktop *target = server.canvas.desktops.in_direction(ox, oy, dir.dx, dir.dy,
		cur != nullptr ? std::optional<DesktopId>{cur->id()} : std::nullopt);

	if (target != nullptr) {
		Point centre = target->centre();
		view->move(static_cast<int>(std::round(view->x + centre.x - ox)),
			static_cast<int>(std::round(view->y + centre.y - oy)));
		output->goto_box(target->frame());
	} else {
		view->move(static_cast<int>(std::round(view->x + dir.dx * output->width)),
			static_cast<int>(std::round(view->y + dir.dy * output->height)));
		server.reveal_view(*view);
	}
}

void act_pan(Server &, Output *output, KeyArg arg) {
	Dir dir = std::get<Dir>(arg);
	if (output != nullptr) {
		output->pan(dir.dx * output->width / 3.0,
			dir.dy * output->height / 3.0);
	}
}

void act_pin(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->pin_desktop();
	}
}

void act_abandon_current(Server &, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	std::optional<DesktopId> desktop;
	if (Desktop *showing = output->desktop_showing()) {
		desktop = showing->id();
	}
	output->overview_abandon(desktop);
}

void act_tile_current(Server &server, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	Desktop *desktop = output->desktop_showing();
	wlr_box box;
	if (desktop != nullptr) {
		box = desktop->frame();
	} else {
		Rect vp = output->viewport();
		box.x = static_cast<int>(std::round(vp.x));
		box.y = static_cast<int>(std::round(vp.y));
		box.width = static_cast<int>(std::round(vp.w));
		box.height = static_cast<int>(std::round(vp.h));
	}
	server.canvas.tile(box, *output);
}

void act_goto_desktop(Server &server, Output *output, KeyArg arg) {
	if (output == nullptr) {
		return;
	}
	Desktop *desktop = server.canvas.desktops.find(DesktopId{std::get<int>(arg)});
	if (desktop != nullptr) {
		output->goto_box(desktop->frame());
	}
}

void act_quit(Server &server, Output *, KeyArg) {
	wl_display_terminate(server.display);
}

void act_vt(Server &server, Output *, KeyArg arg) {
	if (server.session != nullptr) {
		wlr_session_change_vt(server.session,
			static_cast<unsigned>(std::get<int>(arg)));
	}
}

// dwl-style binding table. Keysyms are matched case-insensitively so
// Shift+letter bindings still hit.
constexpr std::array keys = {
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_Return,
		.fn = act_spawn, .arg = config::terminal_cmd},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_space,
		.fn = act_launcher, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_q,
		.fn = act_close, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_ALT, .keysym = XKB_KEY_F4,
		.fn = act_close, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_m,
		.fn = act_minimize, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT,
		.keysym = XKB_KEY_m, .fn = act_restore_minimized, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_Up,
		.fn = act_maximize, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_f,
		.fn = act_fullscreen, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_Tab,
		.fn = act_overview, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_w,
		.fn = act_overview, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL,
		.keysym = XKB_KEY_Left, .fn = act_goto_dir, .arg = Dir{-1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL,
		.keysym = XKB_KEY_Right, .fn = act_goto_dir, .arg = Dir{1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL,
		.keysym = XKB_KEY_Up, .fn = act_goto_dir, .arg = Dir{0, -1}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL,
		.keysym = XKB_KEY_Down, .fn = act_goto_dir, .arg = Dir{0, 1}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT,
		.keysym = XKB_KEY_Left, .fn = act_move_dir, .arg = Dir{-1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT,
		.keysym = XKB_KEY_Right, .fn = act_move_dir, .arg = Dir{1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT,
		.keysym = XKB_KEY_Up, .fn = act_move_dir, .arg = Dir{0, -1}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT,
		.keysym = XKB_KEY_Down, .fn = act_move_dir, .arg = Dir{0, 1}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_Left, .fn = act_pan, .arg = Dir{-1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_Right, .fn = act_pan, .arg = Dir{1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_Up, .fn = act_pan, .arg = Dir{0, -1}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_Down, .fn = act_pan, .arg = Dir{0, 1}},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL,
		.keysym = XKB_KEY_d, .fn = act_pin, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL,
		.keysym = XKB_KEY_F4, .fn = act_abandon_current, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_t,
		.fn = act_tile_current, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_1,
		.fn = act_goto_desktop, .arg = 1},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_2,
		.fn = act_goto_desktop, .arg = 2},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_3,
		.fn = act_goto_desktop, .arg = 3},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_4,
		.fn = act_goto_desktop, .arg = 4},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_5,
		.fn = act_goto_desktop, .arg = 5},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_6,
		.fn = act_goto_desktop, .arg = 6},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_7,
		.fn = act_goto_desktop, .arg = 7},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_8,
		.fn = act_goto_desktop, .arg = 8},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_9,
		.fn = act_goto_desktop, .arg = 9},
	Keybind{.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT,
		.keysym = XKB_KEY_e, .fn = act_quit, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_BackSpace, .fn = act_quit, .arg = 0},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F1, .fn = act_vt, .arg = 1},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F2, .fn = act_vt, .arg = 2},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F3, .fn = act_vt, .arg = 3},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F4, .fn = act_vt, .arg = 4},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F5, .fn = act_vt, .arg = 5},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F6, .fn = act_vt, .arg = 6},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F7, .fn = act_vt, .arg = 7},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F8, .fn = act_vt, .arg = 8},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F9, .fn = act_vt, .arg = 9},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F10, .fn = act_vt, .arg = 10},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F11, .fn = act_vt, .arg = 11},
	Keybind{.mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
		.keysym = XKB_KEY_F12, .fn = act_vt, .arg = 12},
};

bool handle_global_keys(Server &server, Output *output,
		std::span<const xkb_keysym_t> syms, uint32_t mods) {
	for (xkb_keysym_t raw : syms) {
		xkb_keysym_t sym = xkb_keysym_to_lower(raw);
		for (const Keybind &key : keys) {
			if (key.keysym == sym && key.mods == mods) {
				key.fn(server, output, key.arg);
				return true;
			}
		}
	}
	return false;
}

void overview_new_desktop_at(Server &server, Output &output) {
	if (server.canvas.desktops.find(output.target_desktop) == nullptr) {
		// Free spot: plant the new desktop on the target's centre.
		output.overview_new_desktop(output.target.x + output.target.width / 2.0,
			output.target.y + output.target.height / 2.0);
		return;
	}
	std::optional<CanvasPos> pos = server.input.cursor_canvas_pos();
	if (pos.has_value()) {
		output.overview_new_desktop(pos->cx, pos->cy);
	} else {
		output.overview_new_desktop(output.target.x + output.target.width / 2.0,
			output.target.y + output.target.height / 2.0);
	}
}

// Keys while the output under the cursor is in the overview. Every press
// is compositor-owned; callers swallow unmatched ones too.
void handle_overview_keys(Server &server, Output &output,
		std::span<const xkb_keysym_t> syms, uint32_t mods) {
	for (xkb_keysym_t raw : syms) {
		xkb_keysym_t sym = xkb_keysym_to_lower(raw);
		switch (sym) {
		case XKB_KEY_Escape:
			output.overview_cancel();
			return;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			output.overview_exit();
			return;
		case XKB_KEY_Left:
			output.overview_hop(-1, 0);
			return;
		case XKB_KEY_Right:
			output.overview_hop(1, 0);
			return;
		case XKB_KEY_Up:
			output.overview_hop(0, -1);
			return;
		case XKB_KEY_Down:
			output.overview_hop(0, 1);
			return;
		case XKB_KEY_Delete:
		case XKB_KEY_BackSpace:
			if (Desktop *desktop = server.canvas.desktops.find(output.target_desktop)) {
				output.overview_abandon(desktop->id());
			}
			return;
		case XKB_KEY_t:
			output.overview_tile(output.target_desktop);
			return;
		case XKB_KEY_m:
			output.overview_combine(output.target_desktop);
			return;
		case XKB_KEY_n:
			overview_new_desktop_at(server, output);
			return;
		case XKB_KEY_plus:
		case XKB_KEY_equal:
			output.overview_zoom_by(1.25, output.width / 2.0,
				output.height / 2.0);
			return;
		case XKB_KEY_minus:
			output.overview_zoom_by(0.8, output.width / 2.0,
				output.height / 2.0);
			return;
		case XKB_KEY_Tab:
		case XKB_KEY_w:
			if (mods & WLR_MODIFIER_LOGO) {
				output.overview_exit();
			}
			return;
		default:
			break;
		}
	}
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
	double sx = 0, sy = 0;           // surface-local coordinates
	DecoPart part = DecoPart::none;  // decoration part when surface == null
	uint32_t edges = 0;              // resize edges for DecoPart::resize
};

PointerHit pointer_hit_test(Server &server, Output &output) {
	PointerHit hit;
	double ox = server.cursor->x - output.lx;
	double oy = server.cursor->y - output.ly;

	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			l >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; l--) {
		std::optional<LayerHit> layer_hit = output.layer_surface_at(l, ox, oy);
		if (layer_hit.has_value()) {
			hit.surface = layer_hit->surface;
			hit.layer = layer_hit->layer;
			hit.sx = layer_hit->sx;
			hit.sy = layer_hit->sy;
			return hit;
		}
	}

	Point p = output.local_to_canvas(ox, oy);
	std::optional<ViewHit> view_hit =
		server.view_at(p.x, p.y, output.camera().zoom);
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

// Target the desktop a view belongs to, or a screen-sized free spot
// centred on the view, and leave the overview onto it.
void overview_target_view(Server &server, Output &output, View &view) {
	Desktop *desktop = server.canvas.desktop_of(view);
	if (desktop != nullptr) {
		output.overview_set_target(desktop->frame(), desktop->id(), true);
	} else {
		wlr_box frame = view.frame_box();
		wlr_box box{
			.x = frame.x + frame.width / 2 - output.width / 2,
			.y = frame.y + frame.height / 2 - output.height / 2,
			.width = output.width,
			.height = output.height,
		};
		output.overview_set_target(box, std::nullopt, true);
	}
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

Input::Input(Server &server) : server_(server) {
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
		} else if (output != nullptr && output->mode == Mode::overview) {
			handle_overview_keys(server, *output, sym_list, mods);
			handled = true;
		} else if (nsyms > 0) {
			handled = handle_global_keys(server, output, sym_list, mods);
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

// ---- compositor grabs ----

void Input::begin_move(View &view, Output &output) {
	cursor_mode = CursorMode::move;
	grab_view = &view;
	grab_output = &output;
	grab_cam = output.camera();
	grab_box = view.content_box();
	grab_lx = server_.cursor->x;
	grab_ly = server_.cursor->y;
	Point p = output.local_to_canvas(server_.cursor->x - output.lx,
		server_.cursor->y - output.ly);
	grab_cx = p.x;
	grab_cy = p.y;
	grab_moved = false;
	if (view.maximized) {
		// Remember where on the title bar the grab started so that the
		// drag-restore below can keep the cursor at the same spot.
		wlr_box frame = view.frame_box();
		move_rel_x_ = frame.width > 0
			? clampd((grab_cx - frame.x) / frame.width, 0.0, 1.0) : 0.5;
		move_tb_off_ = clampd(grab_cy - frame.y, 0.0,
			double(view.deco.titlebar()));
	}
}

void Input::begin_resize(View &view, Output &output, uint32_t edges) {
	if (edges == 0) {
		edges = WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
	}
	cursor_mode = CursorMode::resize;
	grab_view = &view;
	grab_output = &output;
	grab_cam = output.camera();
	grab_box = view.content_box();
	grab_lx = server_.cursor->x;
	grab_ly = server_.cursor->y;
	grab_edges = edges;
	grab_moved = false;
	// view.cpp anchors the moving (left/top) edges on commit while resizing.
	view.resize_edges = edges;
	view.resize_end_serial = 0;
	view.resize_start = grab_box;
	if (view.wlr != nullptr) {
		wlr_xdg_toplevel_set_resizing(view.wlr, true);
	}
	set_cursor_image(wlr_xcursor_get_resize_name(static_cast<wlr_edges>(edges)));
}

void Input::begin_pan(Output &output) {
	cursor_mode = CursorMode::pan;
	grab_output = &output;
	grab_cam = output.camera();
	grab_lx = server_.cursor->x;
	grab_ly = server_.cursor->y;
	grab_moved = false;
}

void Input::begin_deco_button(View &view, Output &output, DecoPart part) {
	cursor_mode = CursorMode::deco_button;
	grab_view = &view;
	grab_output = &output;
	grab_part = part;
	grab_lx = server_.cursor->x;
	grab_ly = server_.cursor->y;
	grab_moved = false;
	view.deco.pressed = part;
	view.damage();
}

void Input::process_move_grab() {
	View *view = grab_view;
	Output *output = grab_output;
	if (view == nullptr || output == nullptr) {
		cancel_grab();
		return;
	}
	double dist = std::hypot(server_.cursor->x - grab_lx,
		server_.cursor->y - grab_ly);
	if (view->maximized) {
		if (dist < config::drag_threshold) {
			return;
		}
		// Restore under the cursor: same relative x across the title bar,
		// same height into it.
		view->set_maximized(false);
		wlr_box restored = view->saved;
		if (restored.width <= 0 || restored.height <= 0) {
			restored = view->content_box();
		}
		int border = view->deco.border();
		int title = view->deco.titlebar();
		Point p = output->local_to_canvas(server_.cursor->x - output->lx,
			server_.cursor->y - output->ly);
		double frame_w = restored.width + 2.0 * border;
		int frame_left = static_cast<int>(std::round(p.x - move_rel_x_ * frame_w));
		int frame_top = static_cast<int>(std::round(p.y - move_tb_off_));
		view->move(frame_left + border, frame_top + title + border);
		grab_box.x = view->x;
		grab_box.y = view->y;
		grab_box.width = restored.width;
		grab_box.height = restored.height;
		grab_lx = server_.cursor->x;
		grab_ly = server_.cursor->y;
		grab_moved = true;
		return;
	}
	if (dist >= config::drag_threshold) {
		grab_moved = true;
	}
	double dx = (server_.cursor->x - grab_lx) / grab_cam.zoom;
	double dy = (server_.cursor->y - grab_ly) / grab_cam.zoom;
	view->move(static_cast<int>(std::round(grab_box.x + dx)),
		static_cast<int>(std::round(grab_box.y + dy)));
}

void Input::process_resize_grab() {
	View *view = grab_view;
	Output *output = grab_output;
	if (view == nullptr || view->wlr == nullptr || output == nullptr) {
		return;
	}
	double dx = (server_.cursor->x - grab_lx) / grab_cam.zoom;
	double dy = (server_.cursor->y - grab_ly) / grab_cam.zoom;
	double x1 = grab_box.x;
	double y1 = grab_box.y;
	double x2 = x1 + grab_box.width;
	double y2 = y1 + grab_box.height;
	if (grab_edges & WLR_EDGE_LEFT) {
		x1 += dx;
	}
	if (grab_edges & WLR_EDGE_RIGHT) {
		x2 += dx;
	}
	if (grab_edges & WLR_EDGE_TOP) {
		y1 += dy;
	}
	if (grab_edges & WLR_EDGE_BOTTOM) {
		y2 += dy;
	}

	wlr_xdg_toplevel *toplevel = view->wlr;
	double min_w = 100, min_h = 60;
	if (toplevel->current.min_width > min_w) {
		min_w = toplevel->current.min_width;
	}
	if (toplevel->current.min_height > min_h) {
		min_h = toplevel->current.min_height;
	}
	double max_w = toplevel->current.max_width > 0
		? toplevel->current.max_width : 1e9;
	double max_h = toplevel->current.max_height > 0
		? toplevel->current.max_height : 1e9;
	double w = clampd(x2 - x1, min_w, max_w);
	double h = clampd(y2 - y1, min_h, max_h);
	wlr_xdg_toplevel_set_size(toplevel, static_cast<int32_t>(std::round(w)),
		static_cast<int32_t>(std::round(h)));
}

void Input::process_pan_grab() {
	Output *output = grab_output;
	if (output == nullptr) {
		return;
	}
	// Content follows the cursor.
	Camera cam = grab_cam;
	cam.cx -= (server_.cursor->x - grab_lx) / cam.zoom;
	cam.cy -= (server_.cursor->y - grab_ly) / cam.zoom;
	output->set_camera(cam);
}

void Input::process_deco_button_grab() {
	View *view = grab_view;
	if (view == nullptr) {
		return;
	}
	std::optional<CanvasPos> pos = cursor_canvas_pos();
	if (!pos.has_value()) {
		return;
	}
	DecoHit hit = view->deco.hit(pos->cx, pos->cy, pos->output->camera().zoom);
	DecoPart pressed = hit.part == grab_part ? grab_part : DecoPart::none;
	if (view->deco.pressed != pressed) {
		view->deco.pressed = pressed;
		view->damage();
	}
}

void Input::process_desktop_drag_grab() {
	Desktop *desktop = server_.canvas.desktops.find(grab_desktop);
	if (desktop == nullptr) {
		// Its desktop was removed mid-drag; nothing follows the cursor.
		return;
	}
	double dx = (server_.cursor->x - grab_lx) / grab_cam.zoom;
	double dy = (server_.cursor->y - grab_ly) / grab_cam.zoom;
	server_.canvas.desktops.move(desktop->id(),
		static_cast<int>(std::round(grab_box.x + dx)),
		static_cast<int>(std::round(grab_box.y + dy)));

	// Combine when another desktop is overlapped by at least half of the
	// dragged frame; largest overlap wins.
	Rect box = desktop->rect();
	double area = desktop->frame().width * double(desktop->frame().height);
	std::optional<DesktopId> drop;
	double best = 0;
	for (const Desktop &d : server_.canvas.desktops.all()) {
		if (d.id() == desktop->id()) {
			continue;
		}
		double overlap = box.overlap_area(d.rect());
		if (overlap >= config::combine_overlap * area && overlap > best) {
			best = overlap;
			drop = d.id();
		}
	}
	drop_desktop = drop;
}

void Input::process_overview_press_grab() {
	double dist = std::hypot(server_.cursor->x - grab_lx,
		server_.cursor->y - grab_ly);
	if (dist < config::drag_threshold) {
		return;
	}
	grab_moved = true;
	switch (grab_hit) {
	case OverviewHit::desktop:
		if (Desktop *desktop = server_.canvas.desktops.find(grab_desktop)) {
			cursor_mode = CursorMode::desktop_drag;
			grab_box = desktop->frame();
			grab_lx = server_.cursor->x;
			grab_ly = server_.cursor->y;
			drop_desktop = std::nullopt;
		}
		break;
	case OverviewHit::view:
		if (grab_view != nullptr && grab_output != nullptr) {
			begin_move(*grab_view, *grab_output);
		}
		break;
	case OverviewHit::empty:
		if (grab_output != nullptr) {
			begin_pan(*grab_output);
		}
		break;
	default:
		// Tab buttons and shelf items stay clicks.
		break;
	}
}

void Input::process_grab_motion() {
	// Grabs begun outside input.cpp (xdg request_move/request_resize from CSD
	// clients) fill the grab fields but not grab_cam; capture it on the
	// first motion. cancel_grab zeroes it, so it is never stale.
	if (grab_output != nullptr && grab_cam.zoom <= 0) {
		grab_cam = grab_output->camera();
	}
	switch (cursor_mode) {
	case CursorMode::move:
		process_move_grab();
		break;
	case CursorMode::resize:
		process_resize_grab();
		break;
	case CursorMode::pan:
		process_pan_grab();
		break;
	case CursorMode::deco_button:
		process_deco_button_grab();
		break;
	case CursorMode::overview_press:
		process_overview_press_grab();
		break;
	case CursorMode::desktop_drag:
		process_desktop_drag_grab();
		break;
	default:
		break;
	}
}

void Input::cancel_grab() {
	if (cursor_mode == CursorMode::passthrough) {
		return;
	}
	View *view = grab_view;
	if (view != nullptr) {
		view->deco.pressed = DecoPart::none;
		if (view->resize_edges != 0 && view->resize_end_serial == 0) {
			// view.cpp keeps anchoring until the client acks this configure.
			view->resize_end_serial =
				wlr_xdg_toplevel_set_resizing(view->wlr, false);
		}
		view->damage();
	}
	cursor_mode = CursorMode::passthrough;
	grab_view = nullptr;
	grab_desktop = std::nullopt;
	drop_desktop = std::nullopt;
	grab_output = nullptr;
	grab_moved = false;
	grab_cam.zoom = 0; // invalidated; owner re-fills on next grab
	server_.damage_all();
	set_cursor_image("default");
}

// ---- pointer motion ----

void Input::process_cursor_motion(uint32_t time) {
	Server &server = server_;
	if (cursor_mode != CursorMode::passthrough) {
		process_grab_motion();
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
		// Overview / animating: the compositor owns the pointer. Keep the
		// title bar hover state of the view under the cursor alive so the
		// buttons light up, but no client sees any event.
		wlr_seat_pointer_notify_clear_focus(server.seat);
		Camera cam = output->camera();
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
	if (cursor_mode != CursorMode::passthrough) {
		return;
	}
	process_cursor_motion(static_cast<uint32_t>(now_ms()));
}

// ---- overview pointer interaction ----

// Records what an overview press landed on, following the render order:
// shelf item, tab buttons, tab body, window, desktop interior, empty.
void Input::record_overview_hit(Output &output, double ox, double oy,
		double cx, double cy, double zoom) {
	Shelf shelf = overview_shelf_layout(output);
	for (const ShelfItem &item : shelf) {
		if (box_contains(item.box, ox, oy)) {
			grab_hit = OverviewHit::shelf;
			grab_view = item.view;
			return;
		}
	}

	for (const Desktop &desktop : server_.canvas.desktops.all()) {
		std::optional<TabLayout> tab = server_.painter.overview_tab_layout(output, desktop);
		if (!tab.has_value()) {
			continue;
		}
		if (box_contains(tab->tile_button, ox, oy)) {
			grab_hit = OverviewHit::tab_tile;
			grab_desktop = desktop.id();
			return;
		}
		if (box_contains(tab->abandon_button, ox, oy)) {
			grab_hit = OverviewHit::tab_abandon;
			grab_desktop = desktop.id();
			return;
		}
		if (box_contains(tab->tab, ox, oy)) {
			grab_hit = OverviewHit::desktop;
			grab_desktop = desktop.id();
			return;
		}
	}

	std::optional<ViewHit> hit = server_.view_at(cx, cy, zoom);
	if (hit.has_value()) {
		grab_hit = OverviewHit::view;
		grab_view = hit->view;
		return;
	}

	if (Desktop *desktop = server_.canvas.desktops.at(cx, cy)) {
		grab_hit = OverviewHit::desktop;
		grab_desktop = desktop->id();
		return;
	}
	grab_hit = OverviewHit::empty;
}

void Input::overview_click() {
	Output *output = grab_output;
	if (output == nullptr) {
		return;
	}
	View *view = grab_view;
	switch (grab_hit) {
	case OverviewHit::shelf:
		if (view == nullptr) {
			break;
		}
		view->set_minimized(false);
		server_.focus_view(view);
		overview_target_view(server_, *output, *view);
		output->overview_exit();
		break;
	case OverviewHit::tab_tile:
		if (Desktop *desktop = server_.canvas.desktops.find(grab_desktop)) {
			output->overview_tile(desktop->id());
		}
		break;
	case OverviewHit::tab_abandon:
		if (Desktop *desktop = server_.canvas.desktops.find(grab_desktop)) {
			output->overview_abandon(desktop->id());
		}
		break;
	case OverviewHit::desktop:
		if (Desktop *desktop = server_.canvas.desktops.find(grab_desktop)) {
			output->overview_set_target(desktop->frame(), desktop->id(), true);
			output->overview_exit();
		}
		break;
	case OverviewHit::view:
		if (view == nullptr) {
			break;
		}
		server_.focus_view(view);
		overview_target_view(server_, *output, *view);
		output->overview_exit();
		break;
	case OverviewHit::empty: {
		std::optional<CanvasPos> pos = cursor_canvas_pos();
		if (pos.has_value()) {
			output->overview_target_point(pos->cx, pos->cy);
			output->overview_exit();
		}
		break;
	}
	default:
		break;
	}
}

void Input::overview_press(Output &output, uint32_t button) {
	Camera cam = output.camera();
	double ox = server_.cursor->x - output.lx;
	double oy = server_.cursor->y - output.ly;
	Point p = output.local_to_canvas(ox, oy);

	if (button == BTN_RIGHT) {
		// Right click on the empty canvas or a desktop's interior (not
		// over a window) creates a desktop there.
		if (!server_.view_at(p.x, p.y, cam.zoom).has_value()) {
			output.overview_new_desktop(p.x, p.y);
		}
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}

	cursor_mode = CursorMode::overview_press;
	grab_output = &output;
	grab_cam = cam;
	grab_lx = server_.cursor->x;
	grab_ly = server_.cursor->y;
	grab_cx = p.x;
	grab_cy = p.y;
	grab_view = nullptr;
	grab_desktop = std::nullopt;
	grab_moved = false;
	record_overview_hit(output, ox, oy, p.x, p.y, cam.zoom);
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
	if (cursor_mode != CursorMode::passthrough) {
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
		overview_press(*output, button);
		return;
	}

	bool super = seat_modifiers(server) & WLR_MODIFIER_LOGO;
	PointerHit hit = pointer_hit_test(server, *output);

	if (hit.view != nullptr && super && button == BTN_LEFT) {
		server.focus_view(hit.view);
		begin_move(*hit.view, *output);
		return;
	}
	if (hit.view != nullptr && super && button == BTN_RIGHT) {
		server.focus_view(hit.view);
		Point p = output->local_to_canvas(server.cursor->x - output->lx,
			server.cursor->y - output->ly);
		begin_resize(*hit.view, *output, quadrant_edges(*hit.view, p.x, p.y));
		return;
	}

	if (hit.surface != nullptr) {
		if (hit.view != nullptr) {
			server.focus_view(hit.view);
		} else if (hit.layer != nullptr && hit.layer->wlr->current
				.keyboard_interactive
				!= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			server.focus_keyboard(hit.surface);
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
					begin_move(*hit.view, *output);
				}
			}
			return;
		case DecoPart::minimize:
		case DecoPart::maximize:
		case DecoPart::close:
			if (button == BTN_LEFT) {
				begin_deco_button(*hit.view, *output, hit.part);
			}
			return;
		case DecoPart::resize:
			if (button == BTN_LEFT || button == BTN_RIGHT) {
				begin_resize(*hit.view, *output, hit.edges);
			}
			return;
		default:
			return;
		}
	}

	if (button == BTN_LEFT) {
		begin_pan(*output);
	}
}

void Input::release_deco_button() {
	View *view = grab_view;
	if (view == nullptr) {
		return;
	}
	std::optional<CanvasPos> pos = cursor_canvas_pos();
	if (pos.has_value()) {
		DecoHit hit = view->deco.hit(pos->cx, pos->cy,
			pos->output->camera().zoom);
		if (hit.part == grab_part) {
			switch (hit.part) {
			case DecoPart::minimize:
				view->set_minimized(true);
				break;
			case DecoPart::maximize:
				view->set_maximized(!view->maximized);
				break;
			case DecoPart::close:
				view->close();
				break;
			default:
				break;
			}
		}
	}
	if (view->deco.pressed != DecoPart::none) {
		view->deco.pressed = DecoPart::none;
		view->damage();
	}
}

void Input::cursor_button_release(uint32_t time, uint32_t button) {
	if (cursor_mode == CursorMode::passthrough) {
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
	switch (cursor_mode) {
	case CursorMode::deco_button:
		release_deco_button();
		break;
	case CursorMode::overview_press:
		overview_click();
		break;
	case CursorMode::desktop_drag: {
		Desktop *drop = server_.canvas.desktops.find(drop_desktop);
		Desktop *grabbed = server_.canvas.desktops.find(grab_desktop);
		if (drop != nullptr && grabbed != nullptr) {
			server_.canvas.combine(drop->id(), grabbed->id(), grab_box);
		} else if (grabbed != nullptr) {
			server_.canvas.desktops.raise(grabbed->id());
		}
		break;
	}
	default:
		break;
	}
	cancel_grab();
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
	if (cursor_mode != CursorMode::passthrough) {
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
	if (output->mode == Mode::overview) {
		if (seat_modifiers(server) & WLR_MODIFIER_CTRL) {
			output->overview_zoom_by(std::exp(-event->delta / 200.0),
				server.cursor->x - output->lx, server.cursor->y - output->ly);
		} else {
			double delta = event->delta * config::scroll_pan_speed;
			Camera cam = output->camera();
			if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
				cam.cy += delta / cam.zoom;
			} else {
				cam.cx += delta / cam.zoom;
			}
			output->set_camera(cam);
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
		double delta = event->delta * config::scroll_pan_speed;
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
	server_.gesture.swipe_begin(event->time_msec, event->fingers);
}

void Input::handle_swipe_update(wlr_pointer_swipe_update_event *event) {
	notify_activity(server_);
	server_.gesture.swipe_update(event->time_msec, event->dx, event->dy);
}

void Input::handle_swipe_end(wlr_pointer_swipe_end_event *event) {
	notify_activity(server_);
	server_.gesture.swipe_end(event->time_msec, event->cancelled);
}

void Input::handle_pinch_begin(wlr_pointer_pinch_begin_event *event) {
	notify_activity(server_);
	super_tap_ = false;
	server_.gesture.pinch_begin(event->time_msec, event->fingers);
}

void Input::handle_pinch_update(wlr_pointer_pinch_update_event *event) {
	notify_activity(server_);
	server_.gesture.pinch_update(event->time_msec, event->dx, event->dy,
		event->scale, event->rotation);
}

void Input::handle_pinch_end(wlr_pointer_pinch_end_event *event) {
	notify_activity(server_);
	server_.gesture.pinch_end(event->time_msec, event->cancelled);
}

void Input::handle_hold_begin(wlr_pointer_hold_begin_event *event) {
	notify_activity(server_);
	server_.gesture.hold_begin(event->time_msec, event->fingers);
}

void Input::handle_hold_end(wlr_pointer_hold_end_event *event) {
	notify_activity(server_);
	server_.gesture.hold_end(event->time_msec, event->cancelled);
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
