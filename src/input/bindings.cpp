// bindings.cpp: the actions behind the keyboard shortcuts and how every
// resolved chord (core/settings.cpp) reaches them — the tables the settings
// build, the chords that reach everywhere tried before all else, and the
// fixed VT switching no chord can take.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <variant>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

#include "input/bindings.h"
#include "core/chord.h"
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/process.h"
#include "core/settings_file.h"

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

// Super+C and Super+V: the surface with the keyboard is sent the clipboard
// chord — Ctrl+Insert to copy, Shift+Insert to paste, the keys GTK, Qt,
// Chrome and Electron apps take natively and terminals are set to take —
// with only that modifier held (not the Super of the shortcut), then the
// keyboard's real modifiers again.
void act_clipboard_chord(Server &server, Output *, KeyArg arg) {
	wlr_seat *seat = server.seat;
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard == nullptr || seat->keyboard_state.focused_surface == nullptr) {
		return;
	}
	xkb_mod_index_t mod = xkb_keymap_mod_get_index(keyboard->keymap,
		std::get<int>(arg) != 0 ? XKB_MOD_NAME_SHIFT : XKB_MOD_NAME_CTRL);
	if (mod == XKB_MOD_INVALID) {
		return;
	}
	wlr_keyboard_modifiers chord{};
	chord.depressed = 1u << mod;
	uint32_t now = static_cast<uint32_t>(now_ms());
	wlr_seat_keyboard_notify_modifiers(seat, &chord);
	wlr_seat_keyboard_notify_key(seat, now, KEY_INSERT,
		WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(seat, now, KEY_INSERT,
		WL_KEYBOARD_KEY_STATE_RELEASED);
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->modifiers);
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
		view->set_maximized(view->mode != WindowMode::maximized);
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
	if (view->board_seat.has_value() && view->mode == WindowMode::normal) {
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
		view->set_fullscreen(view->mode != WindowMode::fullscreen);
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

void act_pan_back(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->pan_back();
	}
}

void act_pin(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->pin_desktop();
	}
}

void act_abandon_current(Server &server, Output *output, KeyArg) {
	if (output == nullptr) {
		return;
	}
	std::optional<DesktopId> desktop;
	if (Desktop *showing = output->desktop_showing()) {
		desktop = showing->id();
	}
	server.canvas.abandon(desktop);
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
	int number = std::get<int>(arg);
	Desktop *desktop = server.canvas.desktops.by_number(number);
	if (desktop == nullptr) {
		// A missing desktop is made beside desktop 1, then gone to the same way.
		desktop = &server.canvas.desktops.create_beside(number, *output);
	}
	output->goto_desktop(*desktop);
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

void act_lock(Server &server, Output *, KeyArg) {
	server.lock.request("the lock shortcut");
}

// The night light on or off, written into canvas.conf so the choice holds
// across a restart and the settings window shows it.
void act_night_light(Server &server, Output *, KeyArg) {
	settings::File file{server.conf.text()};
	std::expected<settings::Edit, std::string> edit =
		file.set(settings::night_light, server.conf.resolved.night_light ? 0 : 1);
	if (!edit.has_value() || !server.conf.write(edit->text)) {
		wlr_log(WLR_ERROR, "the night light could not be written to canvas.conf");
	}
}

// A shortcut whose work belongs to a shell plugin: the compositor only
// runs the named action, as `action.run` does.
void act_shell(Server &server, Output *, KeyArg arg) {
	server.ipc_events.emit_action(std::get<const char *>(arg), "{}");
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
		server.canvas.abandon(desktop->id());
	}
}

void act_ov_tiling(Server &, Output *output, KeyArg) {
	if (output != nullptr) {
		output->overview.tile(output->overview.target().desktop);
	}
}

void act_ov_combine(Server &server, Output *output, KeyArg) {
	if (output != nullptr) {
		server.canvas.combine_nearest(output->overview.target().desktop);
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
// canvas.conf writes (core/settings.cpp's registry()). Bindings::apply joins
// each action to its row once and logs an action without one.
struct Runner {
	settings::Scope scope;
	const char *name;
	Action fn;
	KeyArg arg;
};

constexpr std::array runners = {
	Runner{settings::Scope::global, "launcher", act_launcher, 0},
	Runner{settings::Scope::global, "terminal", act_spawn, config::terminal_cmd},
	Runner{settings::Scope::global, "copy", act_clipboard_chord, 0},
	Runner{settings::Scope::global, "paste", act_clipboard_chord, 1},
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
	Runner{settings::Scope::global, "pan-back", act_pan_back, 0},
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
	Runner{settings::Scope::global, "lock", act_lock, 0},
	Runner{settings::Scope::global, "night-light", act_night_light, 0},
	Runner{settings::Scope::global, "notifications-dismiss", act_shell,
		config::notification_dismiss_action},
	Runner{settings::Scope::global, "notifications-dismiss-all", act_shell,
		config::notification_dismiss_all_action},
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

// The index in `runners` of what a registry action runs, or -1.
int runner_index(const settings::ActionDef &def) {
	for (std::size_t i = 0; i < runners.size(); i++) {
		if (runners[i].scope == def.scope && runners[i].name == def.name) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

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

// Runs the VT switch that sym with mods names. True when there was one.
bool run_vt(Server &server, Output *output, xkb_keysym_t sym, uint32_t mods) {
	for (const VtBind &vt : vt_keys) {
		if (vt.keysym == sym && vt.mods == mods) {
			act_vt(server, output, vt.vt);
			return true;
		}
	}
	return false;
}

} // namespace

Bindings::Bindings(Server &server)
	: server_(server) {
}

bool Bindings::has_runner(const settings::ActionDef &def) {
	return runner_index(def) >= 0;
}

Bindings::Table Bindings::table_of(const settings::ActionDef &def) {
	if (def.reach == settings::Reach::everywhere) {
		return Table::everywhere;
	}
	return def.scope == settings::Scope::overview ? Table::overview : Table::global;
}

void Bindings::apply(const settings::Resolved &resolved) {
	global_.clear();
	everywhere_.clear();
	overview_.clear();
	commands_.clear();
	command_lines_.clear();

	// Super+Shift+3 is '#' on a US layout, so the digits are matched
	// through their unshifted level too (input.cpp), and Shift+Print is
	// Sys_Req on most layouts, so both keysyms are bound.
	auto add = [](std::vector<Bind> &table, const settings::Chord &chord,
			Bind bind) {
		bind.mods = chord.mods;
		bind.key = chord.key;
		bind.twice = chord.twice;
		table.push_back(bind);
		if (chord.key == XKB_KEY_Print
				&& (chord.mods & settings::mod_shift) != 0) {
			bind.key = XKB_KEY_Sys_Req;
			bind.twice = false;
			table.push_back(bind);
		}
	};

	const std::vector<settings::ActionDef> &registry = settings::registry();
	for (std::size_t i = 0; i < registry.size(); i++) {
		const settings::ActionDef &def = registry[i];
		int runner = runner_index(def);
		if (runner < 0) {
			wlr_log(WLR_ERROR, "the action %.*s has no runner: its keys do nothing",
				static_cast<int>(def.name.size()), def.name.data());
			continue;
		}
		std::vector<Bind> *table = &global_;
		switch (table_of(def)) {
		case Table::everywhere:
			table = &everywhere_;
			break;
		case Table::overview:
			table = &overview_;
			break;
		case Table::global:
			break;
		}
		for (const settings::Chord &chord : resolved.keys[i]) {
			// `key` 0 is the Super tap or double tap: handle_tap runs it,
			// find never matches it.
			add(*table, chord, {.runner = runner,
				.in_overview = def.reach == settings::Reach::overview});
		}
	}
	for (const settings::Command &command : resolved.commands) {
		command_lines_.push_back(command.command);
		add(commands_, command.chord,
			{.command = static_cast<int>(command_lines_.size() - 1)});
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

void Bindings::run(const Bind &bind, Output *output) {
	if (bind.command >= 0) {
		act_spawn(server_, output, command_lines_[bind.command].c_str());
		return;
	}
	const Runner &runner = runners[bind.runner];
	runner.fn(server_, output, runner.arg);
}

bool Bindings::run_found(const std::vector<Bind> &table, xkb_keysym_t sym,
		uint32_t mods, Output *output) {
	const Bind *bind = find(sym, mods, table);
	if (bind == nullptr) {
		return false;
	}
	run(*bind, output);
	return true;
}

bool Bindings::handle(xkb_keysym_t sym, uint32_t mods, Output *output) {
	if (handle_everywhere(sym, mods, output)) {
		return true;
	}
	// The fixed VT switching: no chord can take it.
	if (run_vt(server_, output, sym, mods)) {
		return true;
	}
	return run_found(global_, sym, mods, output)
		|| run_found(commands_, sym, mods, output);
}

bool Bindings::handle_locked(xkb_keysym_t sym, uint32_t mods, Output *output) {
	// Only the way out of a stuck session; every other chord belongs to the
	// locker while the session is locked.
	return run_vt(server_, output, sym, mods);
}

bool Bindings::handle_everywhere(xkb_keysym_t sym, uint32_t mods, Output *output) {
	return run_found(everywhere_, sym, mods, output);
}

bool Bindings::handle_overview(xkb_keysym_t sym, uint32_t mods, Output &output) {
	sym = xkb_keysym_to_lower(sym);
	// The overview's own keys first...
	if (run_found(overview_, sym, mods, &output)) {
		return true;
	}
	// ...then the chords that reach everywhere, the global actions that
	// reach the overview and the [commands] chords. Every other key does
	// nothing there.
	if (handle_everywhere(sym, mods, &output)) {
		return true;
	}
	for (const Bind &bind : global_) {
		if (bind.in_overview && bind.key == sym && bind.mods == mods) {
			run(bind, &output);
			return true;
		}
	}
	return run_found(commands_, sym, mods, &output);
}

const Bindings::Bind *Bindings::tap_bind(Output *output, bool twice) const {
	// The overview's holder first when that output is in the overview,
	// then the global one.
	std::array<const std::vector<Bind> *, 2> tables{
		output != nullptr && output->overview.active() ? &overview_ : nullptr, &global_};
	for (const std::vector<Bind> *table : tables) {
		if (table == nullptr) {
			continue;
		}
		for (const Bind &bind : *table) {
			if (bind.key == 0 && bind.mods == settings::mod_logo && bind.twice == twice) {
				return &bind;
			}
		}
	}
	return nullptr;
}

bool Bindings::handle_tap(Output *output, bool twice) {
	const Bind *bind = tap_bind(output, twice);
	if (bind == nullptr) {
		return false;
	}
	run(*bind, output);
	return true;
}

bool Bindings::has_double_tap(Output *output) const {
	return tap_bind(output, true) != nullptr;
}

bool Bindings::launcher_chord(xkb_keysym_t sym, uint32_t mods) const {
	for (const Bind &bind : global_) {
		if (bind.key != 0 && bind.key == xkb_keysym_to_lower(sym)
				&& bind.mods == mods && bind.runner >= 0
				&& runners[bind.runner].fn == act_launcher) {
			return true;
		}
	}
	return false;
}
