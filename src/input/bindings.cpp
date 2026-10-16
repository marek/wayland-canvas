// bindings.cpp: the keyboard shortcuts and their actions, globally and in
// the overview.

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>

#include "input/bindings.h"
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"

namespace {

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

// Started from the overview, the command's windows open in its target.
void act_spawn(Server &server, Output *output, KeyArg arg) {
	pid_t pid = spawn(std::get<const char *>(arg));
	if (output != nullptr && output->overview.active()) {
		server.launches.add(pid, *output);
	}
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

// The bare-Super arrows: on a tiled desktop they step the focused window's
// slice; on a floating one only Super+Up does anything, toggling maximize.
void act_arrow(Server &server, Output *, KeyArg arg) {
	Dir dir = std::get<Dir>(arg);
	View *view = server.focused_view;
	if (view == nullptr || !view->mapped || view->minimized) {
		return;
	}
	if (view->board.has_value() && !view->maximized && !view->fullscreen) {
		server.canvas.step(*view, dir.dx != 0 ? 0 : 1, dir.dx != 0 ? dir.dx : dir.dy);
		return;
	}
	if (dir.dy < 0) {
		act_maximize(server, nullptr, 0);
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
	if (output->overview.active()) {
		output->overview.exit();
	} else {
		output->overview.enter();
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
		output->goto_desktop(*target);
	} else {
		view->move(static_cast<int>(std::round(view->x + dir.dx * output->width)),
			static_cast<int>(std::round(view->y + dir.dy * output->height)));
		server.reveal_view(*view);
	}
	// The move may have taken the view in or out of a grid.
	server.canvas.settle(*view);
}

void act_pan(Server &, Output *output, KeyArg arg) {
	Dir dir = std::get<Dir>(arg);
	if (output != nullptr) {
		// A third of the screen at the camera's zoom: the canvas moves a
		// third of what it shows.
		double zoom = output->camera.current().zoom;
		output->pan(dir.dx * output->width / (3.0 * zoom),
			dir.dy * output->height / (3.0 * zoom));
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
	output->overview.abandon(desktop);
}

// Super+T: switch tiling for the desktop shown; a free view is pinned as a
// desktop first, like Super+Ctrl+D.
void act_tile_current(Server &server, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	if (output->desktop_showing() == nullptr) {
		output->pin_desktop();
	}
	if (Desktop *desktop = output->desktop_showing()) {
		server.canvas.toggle_tiling(desktop->id());
	}
}

void act_goto_desktop(Server &server, Output *output, KeyArg arg) {
	if (output == nullptr) {
		return;
	}
	Desktop *desktop = server.canvas.desktops.by_number(std::get<int>(arg));
	if (desktop != nullptr) {
		output->goto_desktop(*desktop);
	}
}

// Super+0: the view back to 100 %, the camera easing with it: the desktop
// shown, or a free view where it is.
void act_reset_scale(Server &server, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	if (Desktop *desktop = output->desktop_showing()) {
		server.canvas.settle_scale(desktop->id(), 1.0);
		output->goto_desktop(*desktop);
		return;
	}
	Camera to = output->camera.current();
	to.zoom = 1;
	output->camera.animate_to(to);
	server.hint.set("Free area · 100 %");
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
	// On a tiled desktop the arrows step the focused window's slice; on a
	// floating one Super+Up still toggles maximize.
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_Left,
		.fn = act_arrow, .arg = Dir{-1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_Right,
		.fn = act_arrow, .arg = Dir{1, 0}},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_Up,
		.fn = act_arrow, .arg = Dir{0, -1}},
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_Down,
		.fn = act_arrow, .arg = Dir{0, 1}},
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
	Keybind{.mods = WLR_MODIFIER_LOGO, .keysym = XKB_KEY_0,
		.fn = act_reset_scale, .arg = 0},
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

void overview_new_desktop_at(Server &server, Output &output) {
	const OverviewTarget &target = output.overview.target();
	if (server.canvas.desktops.find(target.desktop) == nullptr) {
		// Free spot: plant the new desktop on the target's centre.
		output.overview.new_desktop(target.box.x + target.box.width / 2.0,
			target.box.y + target.box.height / 2.0);
		return;
	}
	std::optional<CanvasPos> pos = server.input.cursor_canvas_pos();
	if (pos.has_value()) {
		output.overview.new_desktop(pos->cx, pos->cy);
	} else {
		output.overview.new_desktop(target.box.x + target.box.width / 2.0,
			target.box.y + target.box.height / 2.0);
	}
}

} // namespace

Bindings::Bindings(Server &server)
	: server_(server) {
}

bool Bindings::handle(xkb_keysym_t sym, uint32_t mods, Output *output) {
	sym = xkb_keysym_to_lower(sym);
	for (const Keybind &key : keys) {
		if (key.keysym == sym && key.mods == mods) {
			key.fn(server_, output, key.arg);
			return true;
		}
	}
	return false;
}

bool Bindings::handle_overview(xkb_keysym_t sym, uint32_t mods, Output &output) {
	sym = xkb_keysym_to_lower(sym);
	switch (sym) {
	case XKB_KEY_Escape:
		output.overview.cancel();
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		output.overview.exit();
		return true;
	case XKB_KEY_Left:
		output.overview.hop(-1, 0);
		return true;
	case XKB_KEY_Right:
		output.overview.hop(1, 0);
		return true;
	case XKB_KEY_Up:
		output.overview.hop(0, -1);
		return true;
	case XKB_KEY_Down:
		output.overview.hop(0, 1);
		return true;
	case XKB_KEY_Delete:
	case XKB_KEY_BackSpace:
		if (Desktop *desktop =
				server_.canvas.desktops.find(output.overview.target().desktop)) {
			output.overview.abandon(desktop->id());
		}
		return true;
	case XKB_KEY_t:
		output.overview.tile(output.overview.target().desktop);
		return true;
	case XKB_KEY_m:
		output.overview.combine(output.overview.target().desktop);
		return true;
	case XKB_KEY_n:
		overview_new_desktop_at(server_, output);
		return true;
	case XKB_KEY_0:
		if (Desktop *desktop =
				server_.canvas.desktops.find(output.overview.target().desktop)) {
			server_.canvas.settle_scale(desktop->id(), 1.0);
		}
		return true;
	case XKB_KEY_plus:
	case XKB_KEY_equal:
		output.overview.zoom_by(1.25, output.width / 2.0, output.height / 2.0);
		return true;
	case XKB_KEY_minus:
		output.overview.zoom_by(0.8, output.width / 2.0, output.height / 2.0);
		return true;
	case XKB_KEY_Tab:
	case XKB_KEY_w:
		if (mods & WLR_MODIFIER_LOGO) {
			output.overview.exit();
		}
		return true;
	default:
		return false;
	}
}
