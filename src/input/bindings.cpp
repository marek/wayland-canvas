// bindings.cpp: the actions behind the keyboard shortcuts and how every
// resolved chord (core/settings.cpp) reaches them — the tables the settings
// build, the capture chords tried before all else, and the fixed VT
// switching no chord can take.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <variant>

#include <xkbcommon/xkbcommon-keysyms.h>

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

// The capture chords, macOS-style: a shot of the screen, the picker for a
// region or a window, the toolbar, and the recorder's stop. Ctrl on any
// of them copies to the clipboard instead of saving.
void act_shot_screen(Server &server, Output *output, KeyArg arg) {
	if (output != nullptr) {
		server.capture_ui.shoot_screen(*output,
			{.clipboard = std::get<int>(arg) != 0});
	}
}

void act_shot_pick(Server &server, Output *output, KeyArg arg) {
	server.capture_ui.pick_for_shot(output, {.clipboard = std::get<int>(arg) != 0});
}

void act_capture_toolbar(Server &server, Output *output, KeyArg arg) {
	server.capture_ui.open_toolbar(output, false,
		{.clipboard = std::get<int>(arg) != 0});
}

void act_record_stop(Server &server, Output *output, KeyArg arg) {
	server.capture_ui.stop_recording_or_toolbar(output,
		{.clipboard = std::get<int>(arg) != 0});
}

// ------------------------------------------------------- overview actions

void act_ov_hop(Server &, Output *output, KeyArg arg) {
	Dir dir = std::get<Dir>(arg);
	if (output != nullptr) {
		output->overview.hop(dir.dx, dir.dy);
	}
}

void act_ov_go(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.exit();
	}
}

void act_ov_back(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.cancel();
	}
}

void act_ov_abandon(Server &server, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	if (Desktop *desktop =
			server.canvas.desktops.find(output->overview.target().desktop)) {
		output->overview.abandon(desktop->id());
	}
}

void act_ov_tiling(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.tile(output->overview.target().desktop);
	}
}

void act_ov_combine(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.combine(output->overview.target().desktop);
	}
}

void act_ov_new_desktop(Server &server, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	const OverviewTarget &target = output->overview.target();
	if (server.canvas.desktops.find(target.desktop) == nullptr) {
		// Free spot: plant the new desktop on the target's centre.
		output->overview.new_desktop(target.box.x + target.box.width / 2.0,
			target.box.y + target.box.height / 2.0);
		return;
	}
	std::optional<CanvasPos> pos = server.input.cursor_canvas_pos();
	if (pos.has_value()) {
		output->overview.new_desktop(pos->cx, pos->cy);
	} else {
		output->overview.new_desktop(target.box.x + target.box.width / 2.0,
			target.box.y + target.box.height / 2.0);
	}
}

void act_ov_zoom_reset(Server &server, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	if (Desktop *desktop =
			server.canvas.desktops.find(output->overview.target().desktop)) {
		server.canvas.settle_scale(desktop->id(), 1.0);
	}
}

void act_ov_zoom_in(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.zoom_by(1.25, output->width / 2.0, output->height / 2.0);
	}
}

void act_ov_zoom_out(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.zoom_by(0.8, output->width / 2.0, output->height / 2.0);
	}
}

void act_ov_leave(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.exit();
	}
}

// ------------------------------------------------------------ the runners

// What each registry action runs, by scope and name — the same names
// canvas.conf writes (core/settings.cpp's registry()).
struct Runner {
	settings::Scope scope;
	const char *name;
	Action fn;
	KeyArg arg;
};

constexpr std::array runners = {
	Runner{settings::Scope::global, "launcher", act_launcher, 0},
	Runner{settings::Scope::global, "terminal", act_spawn, config::terminal_cmd},
	Runner{settings::Scope::global, "close", act_close, 0},
	Runner{settings::Scope::global, "minimize", act_minimize, 0},
	Runner{settings::Scope::global, "restore", act_restore_minimized, 0},
	Runner{settings::Scope::global, "fullscreen", act_fullscreen, 0},
	Runner{settings::Scope::global, "snap-left", act_arrow, Dir{-1, 0}},
	Runner{settings::Scope::global, "snap-right", act_arrow, Dir{1, 0}},
	Runner{settings::Scope::global, "snap-up", act_arrow, Dir{0, -1}},
	Runner{settings::Scope::global, "snap-down", act_arrow, Dir{0, 1}},
	Runner{settings::Scope::global, "overview", act_overview, 0},
	Runner{settings::Scope::global, "hop-left", act_goto_dir, Dir{-1, 0}},
	Runner{settings::Scope::global, "hop-right", act_goto_dir, Dir{1, 0}},
	Runner{settings::Scope::global, "hop-up", act_goto_dir, Dir{0, -1}},
	Runner{settings::Scope::global, "hop-down", act_goto_dir, Dir{0, 1}},
	Runner{settings::Scope::global, "move-left", act_move_dir, Dir{-1, 0}},
	Runner{settings::Scope::global, "move-right", act_move_dir, Dir{1, 0}},
	Runner{settings::Scope::global, "move-up", act_move_dir, Dir{0, -1}},
	Runner{settings::Scope::global, "move-down", act_move_dir, Dir{0, 1}},
	Runner{settings::Scope::global, "pan-left", act_pan, Dir{-1, 0}},
	Runner{settings::Scope::global, "pan-right", act_pan, Dir{1, 0}},
	Runner{settings::Scope::global, "pan-up", act_pan, Dir{0, -1}},
	Runner{settings::Scope::global, "pan-down", act_pan, Dir{0, 1}},
	Runner{settings::Scope::global, "pin-desktop", act_pin, 0},
	Runner{settings::Scope::global, "abandon-desktop", act_abandon_current, 0},
	Runner{settings::Scope::global, "tiling", act_tile_current, 0},
	Runner{settings::Scope::global, "desktop-1", act_goto_desktop, 1},
	Runner{settings::Scope::global, "desktop-2", act_goto_desktop, 2},
	Runner{settings::Scope::global, "desktop-3", act_goto_desktop, 3},
	Runner{settings::Scope::global, "desktop-4", act_goto_desktop, 4},
	Runner{settings::Scope::global, "desktop-5", act_goto_desktop, 5},
	Runner{settings::Scope::global, "desktop-6", act_goto_desktop, 6},
	Runner{settings::Scope::global, "desktop-7", act_goto_desktop, 7},
	Runner{settings::Scope::global, "desktop-8", act_goto_desktop, 8},
	Runner{settings::Scope::global, "desktop-9", act_goto_desktop, 9},
	Runner{settings::Scope::global, "zoom-reset", act_reset_scale, 0},
	Runner{settings::Scope::global, "shot-screen", act_shot_screen, 0},
	Runner{settings::Scope::global, "shot-screen-copy", act_shot_screen, 1},
	Runner{settings::Scope::global, "capture-pick", act_shot_pick, 0},
	Runner{settings::Scope::global, "capture-pick-copy", act_shot_pick, 1},
	Runner{settings::Scope::global, "capture-toolbar", act_capture_toolbar, 0},
	Runner{settings::Scope::global, "capture-toolbar-copy",
		act_capture_toolbar, 1},
	Runner{settings::Scope::global, "record-stop", act_record_stop, 0},
	Runner{settings::Scope::global, "record-stop-copy", act_record_stop, 1},
	Runner{settings::Scope::global, "quit", act_quit, 0},
	Runner{settings::Scope::overview, "hop-left", act_ov_hop, Dir{-1, 0}},
	Runner{settings::Scope::overview, "hop-right", act_ov_hop, Dir{1, 0}},
	Runner{settings::Scope::overview, "hop-up", act_ov_hop, Dir{0, -1}},
	Runner{settings::Scope::overview, "hop-down", act_ov_hop, Dir{0, 1}},
	Runner{settings::Scope::overview, "go", act_ov_go, 0},
	Runner{settings::Scope::overview, "back", act_ov_back, 0},
	Runner{settings::Scope::overview, "abandon", act_ov_abandon, 0},
	Runner{settings::Scope::overview, "tiling", act_ov_tiling, 0},
	Runner{settings::Scope::overview, "combine", act_ov_combine, 0},
	Runner{settings::Scope::overview, "new-desktop", act_ov_new_desktop, 0},
	Runner{settings::Scope::overview, "zoom-reset", act_ov_zoom_reset, 0},
	Runner{settings::Scope::overview, "zoom-in", act_ov_zoom_in, 0},
	Runner{settings::Scope::overview, "zoom-out", act_ov_zoom_out, 0},
	Runner{settings::Scope::overview, "leave", act_ov_leave, 0},
};

// VT switching is fixed: in no file, and no chord can take it — the way
// out of a stuck session stays.
struct VtBind {
	uint32_t mods;
	xkb_keysym_t keysym;
	int vt;
};

constexpr std::array vt_keys = {
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F1, 1},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F2, 2},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F3, 3},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F4, 4},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F5, 5},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F6, 6},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F7, 7},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F8, 8},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F9, 9},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F10, 10},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F11, 11},
	VtBind{settings::mod_ctrl | settings::mod_alt, XKB_KEY_F12, 12},
};

} // namespace

Bindings::Bindings(Server &server)
	: server_(server) {
}

void Bindings::apply(const settings::Resolved &resolved) {
	global_.clear();
	capture_.clear();
	overview_.clear();
	commands_.clear();
	command_lines_.clear();
	launcher_ = -1;
	terminal_ = -1;

	// Super+Shift+3 is '#' on a US layout, so the digits are matched
	// through their unshifted level too (input.cpp), and Shift+Print is
	// Sys_Req on most layouts, so both keysyms are bound.
	auto add = [](std::vector<Bind> &table, const settings::Chord &chord,
			int action, int command) {
		table.push_back({chord.mods, chord.key, action, command});
		if (chord.key == XKB_KEY_Print
				&& (chord.mods & settings::mod_shift) != 0) {
			table.push_back({chord.mods, XKB_KEY_Sys_Req, action, command});
		}
	};

	const std::vector<settings::ActionDef> &registry = settings::registry();
	for (std::size_t i = 0; i < registry.size(); i++) {
		const settings::ActionDef &def = registry[i];
		int index = static_cast<int>(i);
		if (def.scope == settings::Scope::global && def.name == "launcher") {
			launcher_ = index;
		} else if (def.scope == settings::Scope::global
				&& def.name == "terminal") {
			terminal_ = index;
		}
		for (const settings::Chord &chord : resolved.keys[i]) {
			// The tap (`key` 0) runs from handle_tap, never as a chord.
			if (chord.key == 0) {
				continue;
			}
			if (def.group == "Capture") {
				add(capture_, chord, index, -1);
			} else if (def.scope == settings::Scope::global) {
				add(global_, chord, index, -1);
			} else {
				add(overview_, chord, index, -1);
			}
		}
	}
	for (const settings::Command &command : resolved.commands) {
		command_lines_.push_back(command.command);
		add(commands_, command.chord, -1,
			static_cast<int>(command_lines_.size() - 1));
	}
}

const Bindings::Bind *Bindings::find(xkb_keysym_t sym, uint32_t mods,
		const std::vector<Bind> &table) const {
	for (const Bind &bind : table) {
		// Keysyms match case-insensitively so Shift+letter bindings hit.
		if (bind.key != 0 && bind.key == xkb_keysym_to_lower(sym)
				&& bind.mods == mods) {
			return &bind;
		}
	}
	return nullptr;
}

bool Bindings::run(const Bind &bind, Output *output) {
	if (bind.command >= 0) {
		act_spawn(server_, output, command_lines_[bind.command].c_str());
		return true;
	}
	const settings::ActionDef &def = settings::registry()[bind.action];
	for (const Runner &runner : runners) {
		if (runner.scope == def.scope && runner.name == def.name) {
			runner.fn(server_, output, runner.arg);
			return true;
		}
	}
	return false;
}

bool Bindings::handle(xkb_keysym_t sym, uint32_t mods, Output *output) {
	if (handle_capture(sym, mods, output)) {
		return true;
	}
	sym = xkb_keysym_to_lower(sym);
	// The fixed VT switching: no chord can take it.
	for (const VtBind &vt : vt_keys) {
		if (vt.keysym == sym && vt.mods == mods) {
			act_vt(server_, output, vt.vt);
			return true;
		}
	}
	if (const Bind *bind = find(sym, mods, global_)) {
		return run(*bind, output);
	}
	if (const Bind *bind = find(sym, mods, commands_)) {
		return run(*bind, output);
	}
	return false;
}

bool Bindings::handle_capture(xkb_keysym_t sym, uint32_t mods, Output *output) {
	if (const Bind *bind = find(sym, mods, capture_)) {
		run(*bind, output);
		return true;
	}
	return false;
}

bool Bindings::handle_overview(xkb_keysym_t sym, uint32_t mods, Output &output) {
	sym = xkb_keysym_to_lower(sym);
	// The overview's own keys first...
	if (const Bind *bind = find(sym, mods, overview_)) {
		return run(*bind, &output);
	}
	// ...then the capture chords, the launcher, the terminal and the
	// [commands] chords. Every other key does nothing there.
	if (handle_capture(sym, mods, &output)) {
		return true;
	}
	for (const Bind &bind : global_) {
		if (bind.key == sym && bind.mods == mods
				&& (bind.action == launcher_ || bind.action == terminal_)) {
			return run(bind, &output);
		}
	}
	if (const Bind *bind = find(sym, mods, commands_)) {
		return run(*bind, &output);
	}
	return false;
}

bool Bindings::handle_tap(Output *output) {
	// The overview's holder first when that output is in the overview,
	// then the global one.
	for (const std::vector<Bind> *table :
			{output != nullptr && output->overview.active() ? &overview_ : nullptr,
				&global_}) {
		for (const Bind &bind : *table) {
			if (bind.key == 0 && bind.mods == settings::mod_logo) {
				return run(bind, output);
			}
		}
	}
	return false;
}

bool Bindings::launcher_chord(xkb_keysym_t sym, uint32_t mods) const {
	for (const Bind &bind : global_) {
		if (bind.key != 0 && bind.key == xkb_keysym_to_lower(sym)
				&& bind.mods == mods && bind.action == launcher_) {
			return true;
		}
	}
	return false;
}
