// End-to-end launcher scenario through fake keyboard/pointer devices, plus a
// custom desktop entry installed into $XDG_DATA_HOME before the server starts.
// Writes only under $CANVAS_TMP (meson passes the repo's tmp/).
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iterator>
#include <linux/input-event-codes.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>
#include "core/config.h"
#include "core/server.h"
#include "core/settings.h"
#include "harness/check.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/run.h"
#include "platform/output.h"
#include "windows/view.h"

using namespace harness;

namespace {

int views_before = 0;
std::string marker_path; // what the custom entry's command writes
std::string keys_log; // what the key entries append to when launched
std::size_t key_case = 0, keys_seen = 0; // the key case running; launches logged so far
bool key_launched = false; // the case running launched an entry
bool ends_are_markers = false; // see keys_are_the_lists_ends
std::string gsettings_marker; // written by the gsettings stand-in when anything runs it
bool harness_ignores_sigpipe = false; // as this process was started; see main()

int mapped() {
	int n = 0;
	for (View *v : server->views) {
		n += v->mapped;
	}
	return n;
}

// What the custom entry's command wrote into the marker file: its SigIgn
// mask and its XDG_CURRENT_DESKTOP, once both lines are there in full.
struct Marker {
	unsigned long long sigign;
	std::string desktop;
};
std::optional<Marker> read_marker() {
	std::string text = read_text(marker_path);
	std::size_t first_end = text.find('\n');
	if (first_end == std::string::npos || !text.starts_with("SigIgn:")) {
		return std::nullopt;
	}
	std::size_t second_end = text.find('\n', first_end + 1);
	if (second_end == std::string::npos) {
		return std::nullopt;
	}
	return Marker{std::strtoull(text.c_str() + 7, nullptr, 16),
		text.substr(first_end + 1, second_end - first_end - 1)};
}

void open_launcher() {
	chord(KEY_SPACE, mods::super);
}

// What the launcher's keys do, one case per launch: the keys are pressed in
// the freshly opened launcher, then `accept`; the entry it launched logs its
// word, which must be `expect`. "quokka" lists four entries, alpha first.
struct KeyCase {
	std::string_view name;
	void (*press)();
	std::string_view expect;
	uint32_t accept = KEY_ENTER;
	bool whole_list = false; // acts on the unfiltered list: its first and last entries
};

const KeyCase key_cases[] = {
	{"Down twice selects the third result",
		[] { type_text("quokka"); tap(KEY_DOWN); tap(KEY_DOWN); }, "charlie"},
	{"Up on the first result stays on it",
		[] { type_text("quokka"); tap(KEY_UP); }, "alpha"},
	{"Tab moves down and Shift+Tab up",
		[] {
			type_text("quokka");
			tap(KEY_TAB);
			tap(KEY_TAB);
			tap(KEY_TAB);
			chord(KEY_TAB, mods::shift);
		}, "charlie"},
	{"Ctrl+N moves down and Ctrl+P up",
		[] {
			type_text("quokka");
			chord(KEY_N, mods::ctrl);
			chord(KEY_N, mods::ctrl);
			chord(KEY_P, mods::ctrl);
		}, "bravo"},
	{"Ctrl+Shift+N, the capital, moves down too",
		[] { type_text("quokka"); chord(KEY_N, mods::ctrl_shift); }, "bravo"},
	{"Page Down goes to the last result",
		[] { type_text("quokka"); tap(KEY_PAGEDOWN); }, "delta"},
	{"Page Up goes back to the first",
		[] { type_text("quokka"); tap(KEY_PAGEDOWN); tap(KEY_PAGEUP); }, "alpha"},
	{"Backspace removes the last character",
		[] { type_text("quokkaz"); tap(KEY_BACKSPACE); }, "alpha"},
	{"Ctrl+W removes the last word",
		[] { type_text("quokka zzz"); chord(KEY_W, mods::ctrl); }, "alpha"},
	{"Ctrl+Shift+W removes the last word too",
		[] { type_text("quokka zzz"); chord(KEY_W, mods::ctrl_shift); }, "alpha"},
	{"Ctrl+Backspace removes the last word",
		[] { type_text("quokka zzz"); chord(KEY_BACKSPACE, mods::ctrl); }, "alpha"},
	{"Ctrl+U clears the query and the selection",
		[] {
			type_text("quokka");
			tap(KEY_DOWN);
			chord(KEY_U, mods::ctrl);
			type_text("quokka");
		}, "alpha"},
	{"Ctrl+Shift+U clears the query too",
		[] { type_text("quokka"); chord(KEY_U, mods::ctrl_shift); type_text("quokka"); },
		"alpha"},
	{"Ctrl+X types nothing",
		[] { type_text("quokka"); chord(KEY_X, mods::ctrl); }, "alpha"},
	{"Home and End do nothing while a query filters",
		[] { type_text("quokka"); tap(KEY_DOWN); tap(KEY_HOME); tap(KEY_END); }, "bravo"},
	{"Home selects the whole list's first entry",
		[] { tap(KEY_HOME); }, "home", KEY_ENTER, true},
	{"End selects the whole list's last entry",
		[] { tap(KEY_END); }, "end", KEY_ENTER, true},
	{"The keypad Enter launches too",
		[] { type_text("quokka"); }, "alpha", KEY_KPENTER},
};

// The words the key entries logged so far, one per launch.
std::vector<std::string> keys_logged() {
	std::vector<std::string> lines;
	std::string text = read_text(keys_log);
	std::size_t from = 0;
	for (std::size_t end = 0; (end = text.find('\n', from)) != std::string::npos; from = end + 1) {
		lines.push_back(text.substr(from, end - from));
	}
	return lines;
}

Result devices() {
	attach_pointer();
	attach_keyboard();
	return after(300);
}

Result first_run_and_open() {
	// The first run wrote canvas.conf with every default.
	std::string conf = scratch() + "/xdg-config/wayland-canvas/canvas.conf";
	if (std::optional<std::string> text = read_file(conf)) {
		check(*text == settings::template_text(),
			"the first run wrote canvas.conf with every default");
	} else {
		check(false, "canvas.conf was written on the first run");
	}
	// A lone Super tap runs nothing by default; Super+Space opens.
	tap(KEY_LEFTMETA);
	check(!server->launcher.is_open(), "a lone Super tap runs nothing by default");
	open_launcher();
	check(server->launcher.is_open(), "Super+Space opened the launcher");
	shot("open");
	return after(500);
}

Result panel_renders_and_filters() {
	Output *o = server->outputs.under_cursor();
	std::optional<Launcher::Panel> panel = server->launcher.panel(*o);
	int pw = panel ? panel->box.width : 0, ph = panel ? panel->box.height : 0;
	check(panel && pw > 0, std::format("launcher panel rendered ({}x{})", pw, ph));
	type_text("foot");
	shot("filtered");
	return after(500);
}

Result enter_launches() {
	// Opening drew the entries' icons: the icon theme is read in process,
	// never by running a program the loop would wait on.
	check(!file_exists(gsettings_marker), "opening the launcher ran no gsettings program");
	views_before = mapped();
	tap(KEY_ENTER);
	check(!server->launcher.is_open(), "Enter launched the selection and closed the launcher");
	return after(300);
}

Result launched_app_mapped() {
	if (auto wait = wait_for(mapped() > views_before)) {
		return *wait;
	}
	check(mapped() == views_before + 1, "the launched app (foot) mapped a window");
	shot("launched");
	return after(400);
}

// Logo used as a modifier must not open the launcher.
Result logo_as_a_modifier() {
	chord(KEY_T, mods::super);
	check(!server->launcher.is_open(), "Super+T did not open the launcher on release");
	return after(100);
}

// Its own chord closes the open launcher; Escape does too.
Result its_chord_and_escape_close() {
	open_launcher();
	check(server->launcher.is_open(), "Super+Space opened it");
	open_launcher();
	check(!server->launcher.is_open(), "its own chord closed it");
	open_launcher();
	check(server->launcher.is_open(), "Super+Space opened it again");
	tap(KEY_ESC);
	check(!server->launcher.is_open(), "Escape closed it");
	open_launcher();
	type_text("zzqqx");
	shot("no-match");
	return after(500);
}

Result outside_click_closes() {
	click(20, 20);
	check(!server->launcher.is_open(), "a click outside closed it");
	open_launcher();
	type_text("fo");
	return after(300);
}

// Click the first result row: launches it.
Result result_click_launches() {
	Output *o = server->outputs.under_cursor();
	PanelLayout layout = panel_layout(o->width, o->height);
	wlr_box first = layout.row(0);
	views_before = mapped();
	click(first.x + first.width / 2.0, first.y + first.height / 2.0);
	check(!server->launcher.is_open(), "clicking a result launched it and closed the launcher");
	return after(300);
}

Result clicked_app_mapped() {
	if (auto wait = wait_for(mapped() > views_before)) {
		return *wait;
	}
	check(mapped() == views_before + 1, "clicked app mapped a window");
	return after(300);
}

// The custom desktop entry from $XDG_DATA_HOME: find it by name.
Result custom_entry_found() {
	open_launcher();
	check(server->launcher.is_open(), "launcher reopened for the custom entry");
	type_text("zydeco");
	shot("custom-entry");
	return after(300);
}

Result custom_entry_launched() {
	views_before = mapped();
	tap(KEY_ENTER);
	check(!server->launcher.is_open(), "Enter launched the custom entry and closed the launcher");
	return after(400);
}

Result custom_entry_marker() {
	if (auto wait = wait_for(read_marker().has_value())) {
		return *wait;
	}
	std::optional<Marker> marker = read_marker();
	check(marker.has_value(), "the custom entry's Exec wrote the marker file");
	// The compositor ignores SIGPIPE for itself; what it starts must not
	// inherit that ignore.
	unsigned long long ign = marker ? marker->sigign : 0;
	bool ignored = (ign & (1ull << (SIGPIPE - 1))) != 0;
	check(marker && ignored == harness_ignores_sigpipe, std::format(
		"the launched command got SIGPIPE as the compositor found it ({}, SigIgn {:x})",
		harness_ignores_sigpipe ? "ignored" : "default", ign));
	// Apps and the portal they talk to know which desktop they are in.
	check(marker && marker->desktop == "wayland-canvas:wlroots", std::format(
		"the launched command sees XDG_CURRENT_DESKTOP=wayland-canvas:wlroots ({})",
		marker ? marker->desktop : "-"));
	return after(300);
}

Result custom_entry_opened_foot() {
	if (auto wait = wait_for(mapped() > views_before, {.budget_ms = 20'000})) {
		return *wait;
	}
	check(mapped() == views_before + 1, "the custom entry opened foot (window mapped)");
	return after(0);
}

// Keys of the open launcher: each case launches the entry its keys select.
Result keys_are_the_lists_ends() {
	Apps apps;
	apps.load();
	ends_are_markers = !apps.items().empty()
		&& apps.items().front().name == "!Home Marker"
		&& apps.items().back().name == "~End Marker";
	check(ends_are_markers, "the Home and End entries are the whole list's first and last");
	return after(100);
}

Result press_the_keys() {
	const KeyCase &c = key_cases[key_case];
	if (server->launcher.is_open()) {
		tap(KEY_ESC);
	}
	key_launched = !c.whole_list || ends_are_markers;
	if (key_launched) {
		open_launcher();
		c.press();
		tap(c.accept);
	}
	return after(150);
}

Result the_case_launched() {
	const KeyCase &c = key_cases[key_case];
	if (key_launched) {
		if (auto wait = wait_for(keys_logged().size() > keys_seen)) {
			return *wait;
		}
		std::vector<std::string> lines = keys_logged();
		std::string got = lines.size() > keys_seen ? lines.back() : "nothing";
		check(lines.size() == keys_seen + 1 && got == c.expect && !server->launcher.is_open(),
			std::format("{}: launched {} (wanted {})", c.name, got, c.expect));
		keys_seen = lines.size();
	}
	if (++key_case < std::size(key_cases)) {
		return go_to("press the keys", 100);
	}
	return finish();
}

// Installs a spec-valid custom entry into $XDG_DATA_HOME/applications; its
// Exec uses double quotes, writes its signal dispositions into the marker
// file and opens foot.
void install_custom_entry(const std::string &apps) {
	write_file(apps + "/scenario-custom.desktop", std::format(
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Zydeco Marker\n"
		"Exec=sh -c \"grep SigIgn /proc/self/status > {0};"
		" printenv XDG_CURRENT_DESKTOP >> {0}; exec foot -T scenariocustom\"\n",
		marker_path));
}

// Entries for the key checks, each logging its own word into keys_log when
// run: four whose names sort together, one that sorts before every other
// name and one that sorts after.
void install_key_entries(const std::string &apps) {
	struct Entry {
		const char *file;
		const char *name;
		const char *word;
	};
	constexpr Entry entries[] = {
		{"scenario-key-alpha", "Quokka Alpha", "alpha"},
		{"scenario-key-bravo", "Quokka Bravo", "bravo"},
		{"scenario-key-charlie", "Quokka Charlie", "charlie"},
		{"scenario-key-delta", "Quokka Delta", "delta"},
		{"scenario-key-home", "!Home Marker", "home"},
		{"scenario-key-end", "~End Marker", "end"},
	};
	for (const Entry &entry : entries) {
		write_file(std::format("{}/{}.desktop", apps, entry.file), std::format(
			"[Desktop Entry]\nType=Application\nName={}\nExec=sh -c \"echo {} >> {}\"\n",
			entry.name, entry.word, keys_log));
	}
}

// A `gsettings` first on PATH that records that it ran, and the keyfile
// settings backend holding the icon theme the compositor reads instead.
void install_gsettings_stand_in() {
	std::string shims = scratch_dir("shims");
	gsettings_marker = scratch() + "/gsettings-ran";
	write_script(shims + "/gsettings",
		std::format("#!/bin/sh\n: > {}\necho \"'Adwaita'\"\n", gsettings_marker));
	put_first_on_path(shims);
	setenv("GSETTINGS_BACKEND", "keyfile", 1);
	write_file(scratch_dir("xdg-config/glib-2.0/settings") + "/keyfile",
		"[org/gnome/desktop/interface]\nicon-theme='ScenarioTheme'\n");
}

} // namespace

int main() {
	if (!begin("launcher", {})) {
		return 2;
	}
	struct sigaction found{};
	sigaction(SIGPIPE, nullptr, &found);
	harness_ignores_sigpipe = found.sa_handler == SIG_IGN;
	marker_path = scratch() + "/custom-marker";
	keys_log = scratch() + "/keys.log";
	// Before the server starts: the launcher scans $XDG_DATA_HOME/applications first.
	std::string apps = scratch_env("XDG_DATA_HOME", "xdg") + "/applications";
	mkdir_p(apps);
	install_custom_entry(apps);
	install_key_entries(apps);
	install_gsettings_stand_in();
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	return run(options, {
		{"devices", devices},
		{"first run and open", first_run_and_open},
		{"panel renders and filters", panel_renders_and_filters},
		{"enter launches", enter_launches},
		{"launched app mapped", launched_app_mapped},
		{"logo as a modifier", logo_as_a_modifier},
		{"its chord and escape close", its_chord_and_escape_close},
		{"outside click closes", outside_click_closes},
		{"result click launches", result_click_launches},
		{"clicked app mapped", clicked_app_mapped},
		{"custom entry found", custom_entry_found},
		{"custom entry launched", custom_entry_launched},
		{"custom entry marker", custom_entry_marker},
		{"custom entry opened foot", custom_entry_opened_foot},
		{"keys are the list's ends", keys_are_the_lists_ends},
		{"press the keys", press_the_keys},
		{"the case launched", the_case_launched},
	});
}
