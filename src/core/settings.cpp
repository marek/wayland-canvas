// settings.cpp: canvas.conf — the action registry, chord parse, format and
// validate, file parse with an error per bad line, resolution by the
// precedence rules, the first-run template and the in-place edits
// settings.set makes. Pure: no wlroots, no Server; xkbcommon names the keys.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/settings.h"

namespace settings {

namespace {

std::string_view trim(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'
			|| text.front() == '\r')) {
		text.remove_prefix(1);
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t'
			|| text.back() == '\r')) {
		text.remove_suffix(1);
	}
	return text;
}

char lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

char upper(char c) {
	return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool iequals(std::string_view a, std::string_view b) {
	return a.size() == b.size()
		&& std::equal(a.begin(), a.end(), b.begin(),
			[](char x, char y) { return lower(x) == lower(y); });
}

std::vector<std::string_view> split(std::string_view text, char sep) {
	std::vector<std::string_view> parts;
	std::size_t start = 0;
	for (std::size_t sep_at = text.find(sep); sep_at != std::string_view::npos;
			sep_at = text.find(sep, start)) {
		parts.push_back(trim(text.substr(start, sep_at - start)));
		start = sep_at + 1;
	}
	parts.push_back(trim(text.substr(start)));
	return parts;
}

// The keysym's name as the file writes it: the uppercase letter for a letter,
// the digit for a digit, otherwise the keysym name with its first letter
// upper-cased (`Space`, `BackSpace`, `KP_Enter`, `F4`).
std::string key_text(xkb_keysym_t key) {
	char name[64];
	if (xkb_keysym_get_name(key, name, sizeof name) <= 0) {
		return std::format("0x{:x}", static_cast<unsigned>(key));
	}
	std::string_view text = name;
	if (text.size() == 1 && lower(text[0]) >= 'a' && lower(text[0]) <= 'z') {
		return std::string(1, upper(text[0]));
	}
	std::string out{text};
	if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
		out[0] = upper(out[0]);
	}
	return out;
}

std::optional<uint32_t> modifier_bit(std::string_view token) {
	if (iequals(token, "super")) {
		return mod_logo;
	}
	if (iequals(token, "ctrl")) {
		return mod_ctrl;
	}
	if (iequals(token, "alt")) {
		return mod_alt;
	}
	if (iequals(token, "shift")) {
		return mod_shift;
	}
	return std::nullopt;
}

std::string_view modifier_name(uint32_t bit) {
	switch (bit) {
	case mod_logo: return "Super";
	case mod_ctrl: return "Ctrl";
	case mod_alt: return "Alt";
	default: return "Shift";
	}
}

// A key that types no character, so a chord on it needs no Super, Ctrl or Alt.
bool types_no_character(xkb_keysym_t key) {
	if (key >= XKB_KEY_F1 && key <= XKB_KEY_F24) {
		return true;
	}
	switch (key) {
	case XKB_KEY_Print:
	case XKB_KEY_Pause:
	case XKB_KEY_Scroll_Lock:
	case XKB_KEY_Insert:
		return true;
	default:
		break;
	}
	char name[64];
	if (xkb_keysym_get_name(key, name, sizeof name) > 0
			&& std::strncmp(name, "XF86", 4) == 0) {
		return true;
	}
	return false;
}

// `off` is no idle lock at all, written as 0 minutes.
std::optional<int> parse_minutes(std::string_view value) {
	if (value == "off") {
		return 0;
	}
	int out = 0;
	auto [end, err] = std::from_chars(value.data(), value.data() + value.size(), out);
	if (err != std::errc{} || end != value.data() + value.size() || out < 1) {
		return std::nullopt;
	}
	return out;
}

std::optional<int> parse_temperature(std::string_view value) {
	int out = 0;
	auto [end, err] = std::from_chars(value.data(), value.data() + value.size(), out);
	if (err != std::errc{} || end != value.data() + value.size()
			|| out < night_light_min_k || out > night_light_max_k) {
		return std::nullopt;
	}
	return out;
}

std::optional<SwipePreset> parse_swipes(std::string_view value) {
	if (value == "three-finger-hops") {
		return SwipePreset::three_finger_hops;
	}
	if (value == "four-finger-hops") {
		return SwipePreset::four_finger_hops;
	}
	if (value == "off") {
		return SwipePreset::off;
	}
	return std::nullopt;
}

std::string_view swipes_text(SwipePreset preset) {
	switch (preset) {
	case SwipePreset::three_finger_hops: return "three-finger-hops";
	case SwipePreset::four_finger_hops: return "four-finger-hops";
	case SwipePreset::off: return "off";
	}
	return "three-finger-hops";
}

} // namespace

// The registry (the settings contract §3): [shortcuts] scope global first,
// then [overview] with group "In the overview". Defaults are written as
// chord text and parsed once; a default that fails to parse is dropped (and
// the template test catches that).
const std::vector<ActionDef> &registry() {
	static const std::vector<ActionDef> table = [] {
		std::vector<ActionDef> defs;
		auto add = [&](Scope scope, std::string_view group, std::string_view name,
				std::string_view label, std::initializer_list<const char *> keys) {
			std::vector<Chord> chords;
			for (const char *key : keys) {
				if (auto chord = parse_chord(key, scope)) {
					chords.push_back(*chord);
				}
			}
			defs.push_back({scope, name, label, group, std::move(chords)});
		};
		add(Scope::global, "Apps", "launcher", "App launcher", {"Super+Space"});
		add(Scope::global, "Apps", "terminal", "Terminal", {"Super+Return"});
		add(Scope::global, "Clipboard", "copy", "Copy", {"Super+C"});
		add(Scope::global, "Clipboard", "paste", "Paste", {"Super+V"});
		add(Scope::global, "Windows", "close", "Close the window",
			{"Super+Q", "Alt+F4"});
		add(Scope::global, "Windows", "minimize", "Minimize", {"Super+M"});
		add(Scope::global, "Windows", "restore", "Restore the last minimized",
			{"Super+Shift+M"});
		add(Scope::global, "Windows", "fullscreen", "Fullscreen", {"Super+F"});
		add(Scope::global, "Windows", "snap-left", "Snap left", {"Super+Left"});
		add(Scope::global, "Windows", "snap-right", "Snap right", {"Super+Right"});
		add(Scope::global, "Windows", "snap-up", "Snap up (maximize when floating)",
			{"Super+Up"});
		add(Scope::global, "Windows", "snap-down", "Snap down", {"Super+Down"});
		add(Scope::global, "Canvas", "overview", "Overview",
			{"Super+Tab", "Super+W"});
		add(Scope::global, "Canvas", "hop-left", "Hop left", {"Super+Ctrl+Left"});
		add(Scope::global, "Canvas", "hop-right", "Hop right", {"Super+Ctrl+Right"});
		add(Scope::global, "Canvas", "hop-up", "Hop up", {"Super+Ctrl+Up"});
		add(Scope::global, "Canvas", "hop-down", "Hop down", {"Super+Ctrl+Down"});
		add(Scope::global, "Canvas", "move-left", "Move the window left and follow",
			{"Super+Ctrl+Shift+Left"});
		add(Scope::global, "Canvas", "move-right",
			"Move the window right and follow", {"Super+Ctrl+Shift+Right"});
		add(Scope::global, "Canvas", "move-up", "Move the window up and follow",
			{"Super+Ctrl+Shift+Up"});
		add(Scope::global, "Canvas", "move-down", "Move the window down and follow",
			{"Super+Ctrl+Shift+Down"});
		add(Scope::global, "Canvas", "pan-left", "Pan left", {"Super+Alt+Left"});
		add(Scope::global, "Canvas", "pan-right", "Pan right", {"Super+Alt+Right"});
		add(Scope::global, "Canvas", "pan-up", "Pan up", {"Super+Alt+Up"});
		add(Scope::global, "Canvas", "pan-down", "Pan down", {"Super+Alt+Down"});
		add(Scope::global, "Canvas", "pan-back", "Back to where the pans began",
			{"Super Super"});
		add(Scope::global, "Desktops", "pin-desktop", "Pin the view as a desktop",
			{"Super+Ctrl+D"});
		add(Scope::global, "Desktops", "abandon-desktop", "Abandon the desktop",
			{"Super+Ctrl+F4"});
		add(Scope::global, "Desktops", "tiling", "Tiling on or off", {"Super+T"});
		add(Scope::global, "Desktops", "desktop-1", "Desktop 1", {"Super+1"});
		add(Scope::global, "Desktops", "desktop-2", "Desktop 2", {"Super+2"});
		add(Scope::global, "Desktops", "desktop-3", "Desktop 3", {"Super+3"});
		add(Scope::global, "Desktops", "desktop-4", "Desktop 4", {"Super+4"});
		add(Scope::global, "Desktops", "desktop-5", "Desktop 5", {"Super+5"});
		add(Scope::global, "Desktops", "desktop-6", "Desktop 6", {"Super+6"});
		add(Scope::global, "Desktops", "desktop-7", "Desktop 7", {"Super+7"});
		add(Scope::global, "Desktops", "desktop-8", "Desktop 8", {"Super+8"});
		add(Scope::global, "Desktops", "desktop-9", "Desktop 9", {"Super+9"});
		add(Scope::global, "Desktops", "zoom-reset", "Back to 100 %", {"Super+0"});
		add(Scope::global, "Capture", "shot-screen", "Screenshot",
			{"Super+Shift+3", "Shift+Print"});
		add(Scope::global, "Capture", "shot-screen-copy",
			"Screenshot to the clipboard", {"Super+Ctrl+Shift+3", "Ctrl+Shift+Print"});
		add(Scope::global, "Capture", "capture-pick", "Shot of a region or window",
			{"Super+Shift+4", "Print"});
		add(Scope::global, "Capture", "capture-pick-copy",
			"Shot of a region or window to the clipboard",
			{"Super+Ctrl+Shift+4", "Ctrl+Print"});
		add(Scope::global, "Capture", "capture-toolbar", "Capture toolbar",
			{"Super+Shift+5"});
		add(Scope::global, "Capture", "capture-toolbar-copy",
			"Capture toolbar, to the clipboard", {"Super+Ctrl+Shift+5"});
		add(Scope::global, "Capture", "record-stop", "Stop the recording",
			{"Alt+Print"});
		add(Scope::global, "Capture", "record-stop-copy",
			"Stop the recording, to the clipboard", {"Ctrl+Alt+Print"});
		add(Scope::global, "Session", "lock", "Lock the screen", {"Super+L"});
		add(Scope::global, "Session", "notifications-dismiss",
			"Dismiss the newest notification", {"Super+N"});
		add(Scope::global, "Session", "night-light", "Night light on or off",
			{"Super+Ctrl+N"});
		add(Scope::global, "Session", "notifications-dismiss-all",
			"Dismiss every notification", {"Super+Shift+N"});
		add(Scope::global, "Session", "quit", "Quit wayland-canvas",
			{"Super+Shift+E", "Ctrl+Alt+BackSpace"});
		add(Scope::overview, "In the overview", "hop-left", "Hop left", {"Left"});
		add(Scope::overview, "In the overview", "hop-right", "Hop right", {"Right"});
		add(Scope::overview, "In the overview", "hop-up", "Hop up", {"Up"});
		add(Scope::overview, "In the overview", "hop-down", "Hop down", {"Down"});
		add(Scope::overview, "In the overview", "go", "Go to the target",
			{"Return", "KP_Enter"});
		add(Scope::overview, "In the overview", "back", "Back to where you were",
			{"Escape"});
		add(Scope::overview, "In the overview", "abandon", "Abandon the target desktop",
			{"Delete", "BackSpace"});
		add(Scope::overview, "In the overview", "tiling", "Tiling on or off", {"T"});
		add(Scope::overview, "In the overview", "combine", "Combine into the target",
			{"M"});
		add(Scope::overview, "In the overview", "new-desktop", "New desktop", {"N"});
		add(Scope::overview, "In the overview", "zoom-reset",
			"Target back to 100 %", {"0"});
		add(Scope::overview, "In the overview", "zoom-in", "Zoom in",
			{"Plus", "Equal"});
		add(Scope::overview, "In the overview", "zoom-out", "Zoom out", {"Minus"});
		add(Scope::overview, "In the overview", "leave", "Leave the overview",
			{"Super+Tab", "Super+W"});
		return defs;
	}();
	return table;
}

// ------------------------------------------------------------ chords

std::string chord_text(Chord chord) {
	if (chord.tap()) {
		return chord.twice ? "Super Super" : "Super";
	}
	std::string out;
	auto add = [&](std::string_view part) {
		if (!out.empty()) {
			out += '+';
		}
		out += part;
	};
	if (chord.mods & mod_logo) {
		add("Super");
	}
	if (chord.mods & mod_ctrl) {
		add("Ctrl");
	}
	if (chord.mods & mod_alt) {
		add("Alt");
	}
	if (chord.mods & mod_shift) {
		add("Shift");
	}
	add(key_text(chord.key));
	return out;
}

std::expected<Chord, std::string> parse_chord(std::string_view text, Scope scope) {
	// `Super Super`, two lone taps of Super: the double tap. A space is
	// otherwise only ever padding around a `+`.
	std::string_view whole = trim(text);
	if (std::size_t gap = whole.find_first_of(" \t");
			gap != std::string_view::npos && whole.find('+') == std::string_view::npos
			&& iequals(whole.substr(0, gap), "super")
			&& iequals(trim(whole.substr(gap)), "super")) {
		return Chord{.mods = mod_logo, .twice = true};
	}
	std::vector<std::string_view> tokens = split(text, '+');
	if (std::any_of(tokens.begin(), tokens.end(),
			[](std::string_view token) { return token.empty(); })) {
		return std::unexpected(std::string{"an empty chord"});
	}
	uint32_t mods = 0;
	for (std::size_t i = 0; i + 1 < tokens.size(); i++) {
		std::optional<uint32_t> bit = modifier_bit(tokens[i]);
		if (!bit.has_value()) {
			return std::unexpected(std::format("'{}' is not a modifier", tokens[i]));
		}
		if ((mods & *bit) != 0) {
			return std::unexpected(std::format("{} given twice", modifier_name(*bit)));
		}
		mods |= *bit;
	}

	Chord chord;
	if (std::optional<uint32_t> bit = modifier_bit(tokens.back())) {
		if (tokens.size() != 1 || *bit != mod_logo) {
			return std::unexpected(std::string{"only Super may stand alone"});
		}
		chord.mods = mod_logo; // key 0: the tap
	} else {
		std::string name{tokens.back()}; // xkbcommon wants it NUL-terminated
		xkb_keysym_t key = xkb_keysym_from_name(name.c_str(),
			XKB_KEYSYM_CASE_INSENSITIVE);
		if (key == XKB_KEY_NoSymbol) {
			return std::unexpected(std::format("unknown key '{}'", tokens.back()));
		}
		chord = {.mods = mods, .key = key};
	}
	if (scope == Scope::global && !chord.tap()
			&& (chord.mods & (mod_logo | mod_ctrl | mod_alt)) == 0
			&& !types_no_character(chord.key)) {
		return std::unexpected(
			std::format("{} needs Super, Ctrl or Alt", key_text(chord.key)));
	}
	return chord;
}

namespace {

const ActionDef *find_action(std::string_view name, Scope scope) {
	for (const ActionDef &def : registry()) {
		if (def.scope == scope && def.name == name) {
			return &def;
		}
	}
	return nullptr;
}

std::string_view section_name(Scope scope) {
	return scope == Scope::global ? "shortcuts" : "overview";
}

// One `name = value` line of [shortcuts] or [overview]: the entry's chords,
// or an error per bad chord (the rest of the line stands).
void parse_action_line(std::vector<Entry> &entries, std::vector<Error> &errors,
		int line, std::string_view name, std::string_view value, Scope scope) {
	if (find_action(name, scope) == nullptr) {
		errors.push_back({.line = line, .text = std::format("unknown action '{}'", name)});
		return;
	}
	if (value == "none") {
		entries.push_back({.line = line, .name = std::string{name}, .none = true});
		return;
	}
	Entry entry{.line = line, .name = std::string{name}};
	for (std::string_view token : split(value, ',')) {
		if (auto chord = parse_chord(token, scope)) {
			entry.chords.push_back(*chord);
		} else {
			errors.push_back({.line = line,
				.text = std::format("bad chord '{}': {}", token, chord.error())});
		}
	}
	entries.push_back(std::move(entry));
}

// The same chord on two file lines of one scope (actions and commands
// together): the first line keeps it; on the later line it is an error and
// ignored, and the rest of that line stands.
void dedupe(std::vector<Entry> &actions, const std::vector<Entry> &commands,
		std::vector<Error> &errors) {
	std::vector<std::pair<int, Entry *>> lines;
	for (Entry &entry : actions) {
		lines.push_back({entry.line, &entry});
	}
	for (const Entry &entry : commands) {
		lines.push_back({entry.line, const_cast<Entry *>(&entry)});
	}
	std::sort(lines.begin(), lines.end());
	std::vector<std::pair<Chord, int>> claimed;
	for (const auto &[line, entry] : lines) {
		std::vector<Chord> kept;
		for (Chord chord : entry->chords) {
			auto it = std::find_if(claimed.begin(), claimed.end(),
				[&](const std::pair<Chord, int> &c) { return c.first == chord; });
			if (it != claimed.end()) {
				errors.push_back({.line = line, .text = std::format(
					"chord '{}' is already bound on line {}", chord_text(chord), it->second)});
			} else {
				claimed.push_back({chord, line});
				kept.push_back(chord);
			}
		}
		entry->chords = std::move(kept);
	}
}

// ---------------------------------------------------------- the file
//
// The edit side sees the same lines the parser does: a section runs from
// its header to the next line that starts with '['.

bool is_header_line(std::string_view line) {
	std::string_view text = trim(line);
	return !text.empty() && text.front() == '[';
}

// The entry name before '=', or empty for comments, blanks and non-entries.
std::string_view line_name(std::string_view line) {
	std::string_view text = trim(line);
	if (text.empty() || text.front() == '#' || text.front() == '[') {
		return {};
	}
	std::size_t eq = text.find('=');
	if (eq == std::string_view::npos) {
		return {};
	}
	return trim(text.substr(0, eq));
}

// The value after '=' (the parser's kind of trim).
std::string_view line_value(std::string_view line) {
	std::string_view text = trim(line);
	std::size_t eq = text.find('=');
	return eq == std::string_view::npos ? std::string_view{} : trim(text.substr(eq + 1));
}

} // namespace

// ------------------------------------------------------------- parsing

Parsed parse(std::string_view text) {
	Parsed out;
	enum class Section { none, shortcuts, overview, commands, gestures, session, unknown };
	Section section = Section::none;
	int line_no = 0;
	for (std::string_view line : split(text, '\n')) {
		line_no++;
		if (line.empty() || line.front() == '#') {
			continue;
		}
		if (line.front() == '[') {
			if (line.back() != ']') {
				out.errors.push_back({.line = line_no, .text = "not a setting"});
				continue;
			}
			std::string_view name = trim(line.substr(1, line.size() - 2));
			if (name == "shortcuts") {
				section = Section::shortcuts;
			} else if (name == "overview") {
				section = Section::overview;
			} else if (name == "commands") {
				section = Section::commands;
			} else if (name == "gestures") {
				section = Section::gestures;
			} else if (name == "session") {
				section = Section::session;
			} else {
				out.errors.push_back({.line = line_no,
					.text = std::format("unknown section '{}'", name)});
				section = Section::unknown;
			}
			continue;
		}
		std::size_t eq = line.find('=');
		if (eq == std::string_view::npos || eq == 0 || eq + 1 == line.size()) {
			out.errors.push_back({.line = line_no, .text = "not a setting"});
			continue;
		}
		std::string_view name = trim(line.substr(0, eq));
		std::string_view value = trim(line.substr(eq + 1));
		if (name.empty()) {
			out.errors.push_back({.line = line_no, .text = "not a setting"});
			continue;
		}
		switch (section) {
		case Section::none:
			out.errors.push_back({.line = line_no, .text = "a line before any section"});
			break;
		case Section::unknown:
			break; // an unknown section's lines are ignored
		case Section::shortcuts:
			parse_action_line(out.shortcuts, out.errors, line_no, name, value,
				Scope::global);
			break;
		case Section::overview:
			parse_action_line(out.overview, out.errors, line_no, name, value,
				Scope::overview);
			break;
		case Section::commands: {
			auto chord = parse_chord(name, Scope::global);
			if (!chord.has_value()) {
				out.errors.push_back({.line = line_no, .text = std::format(
					"bad chord '{}': {}", name, chord.error())});
				break;
			}
			Entry entry{.line = line_no, .name = std::string{name}};
			entry.chords.push_back(*chord);
			entry.value = std::string{value};
			out.commands.push_back(std::move(entry));
			break;
		}
		case Section::gestures:
			if (name == "swipes") {
				if (!parse_swipes(value).has_value()) {
					out.errors.push_back({.line = line_no, .text =
						"swipes must be three-finger-hops, four-finger-hops or off"});
				} else {
					out.gestures.push_back({.line = line_no, .name = std::string{name},
						.value = std::string{value}});
				}
			} else if (name == "pinch-scale" || name == "invert") {
				if (value != "on" && value != "off") {
					out.errors.push_back({.line = line_no,
						.text = std::format("{} must be on or off", name)});
				} else {
					out.gestures.push_back({.line = line_no, .name = std::string{name},
						.value = std::string{value}});
				}
			} else {
				out.errors.push_back({.line = line_no,
					.text = std::format("unknown setting '{}'", name)});
			}
			break;
		case Section::session:
			if (name == "lock-idle") {
				if (!parse_minutes(value).has_value()) {
					out.errors.push_back({.line = line_no, .text =
						"lock-idle must be minutes (1 or more) or off"});
				} else {
					out.session.push_back({.line = line_no, .name = std::string{name},
						.value = std::string{value}});
				}
			} else if (name == "night-light") {
				if (value != "on" && value != "off") {
					out.errors.push_back({.line = line_no,
						.text = "night-light must be on or off"});
				} else {
					out.session.push_back({.line = line_no, .name = std::string{name},
						.value = std::string{value}});
				}
			} else if (name == "night-light-temperature") {
				if (!parse_temperature(value).has_value()) {
					out.errors.push_back({.line = line_no, .text = std::format(
						"night-light-temperature must be {} to {} K",
						night_light_min_k, night_light_max_k)});
				} else {
					out.session.push_back({.line = line_no, .name = std::string{name},
						.value = std::string{value}});
				}
			} else {
				out.errors.push_back({.line = line_no,
					.text = std::format("unknown setting '{}'", name)});
			}
			break;
		}
	}
	dedupe(out.shortcuts, out.commands, out.errors);
	dedupe(out.overview, {}, out.errors);
	return out;
}

// ---------------------------------------------------------- resolution

Resolved resolve(const Parsed &parsed) {
	Resolved out;
	const std::vector<ActionDef> &reg = registry();
	out.keys.assign(reg.size(), {});
	for (Scope scope : {Scope::global, Scope::overview}) {
		const std::vector<Entry> &section =
			scope == Scope::global ? parsed.shortcuts : parsed.overview;
		// Each action's first line, and every chord the file gives in this
		// scope (a later line of the same action gives nothing: it lost).
		std::vector<const Entry *> mine(reg.size(), nullptr);
		std::vector<Chord> given;
		for (const Entry &entry : section) {
			for (std::size_t i = 0; i < reg.size(); i++) {
				if (reg[i].scope == scope && reg[i].name == entry.name) {
					if (mine[i] == nullptr) {
						mine[i] = &entry;
						given.insert(given.end(), entry.chords.begin(), entry.chords.end());
					}
					break;
				}
			}
		}
		if (scope == Scope::global) {
			for (const Entry &cmd : parsed.commands) {
				for (Chord chord : cmd.chords) {
					given.push_back(chord);
				}
			}
		}
		auto is_given = [&given](Chord chord) {
			return std::find(given.begin(), given.end(), chord) != given.end();
		};
		for (std::size_t i = 0; i < reg.size(); i++) {
			if (reg[i].scope != scope) {
				continue;
			}
			if (mine[i] != nullptr) {
				out.keys[i] = mine[i]->chords;
			} else {
				// A chord the file gives to an action or a command is taken
				// from any action that has it only by default.
				for (Chord chord : reg[i].defaults) {
					if (!is_given(chord)) {
						out.keys[i].push_back(chord);
					}
				}
			}
		}
	}
	for (const Entry &cmd : parsed.commands) {
		if (!cmd.chords.empty()) {
			out.commands.push_back({.chord = cmd.chords.front(), .command = cmd.value});
		}
	}
	bool idle_set = false, night_set = false, temp_set = false;
	for (const Entry &entry : parsed.session) {
		if (entry.name == "lock-idle" && !idle_set) {
			out.lock_idle_minutes = *parse_minutes(entry.value);
			idle_set = true;
		} else if (entry.name == "night-light" && !night_set) {
			out.night_light = entry.value == "on";
			night_set = true;
		} else if (entry.name == "night-light-temperature" && !temp_set) {
			out.night_light_k = *parse_temperature(entry.value);
			temp_set = true;
		}
	}
	bool swipes_set = false, pinch_set = false, invert_set = false;
	for (const Entry &entry : parsed.gestures) {
		if (entry.name == "swipes" && !swipes_set) {
			out.swipes = *parse_swipes(entry.value);
			swipes_set = true;
		} else if (entry.name == "pinch-scale" && !pinch_set) {
			out.pinch_scale = entry.value == "on";
			pinch_set = true;
		} else if (entry.name == "invert" && !invert_set) {
			out.invert = entry.value == "on";
			invert_set = true;
		}
	}
	return out;
}

// ------------------------------------------------------------ template

std::string template_text() {
	std::string out =
		"# wayland-canvas's configuration, written with every default on\n"
		"# first run. Edit it and save it: the running session picks the\n"
		"# change up. The settings window (the gear in the overview toolbar)\n"
		"# edits it line by line, keeping comments.\n"
		"# A line is `action = chord[, chord...]` or `action = none`. A chord\n"
		"# is modifiers (Super, Ctrl, Alt, Shift, always in that order) then\n"
		"# one key joined by `+`, like Super+Q; `Super` alone is a tap of that\n"
		"# key; a deleted line puts that action back on its defaults. The bar\n"
		"# and the toolbar are set in shell.json beside this file (bar.hidden,\n"
		"# bar.layout, toolbar.position, toolbar.order).\n"
		"\n";
	std::optional<Scope> emitted;
	std::string_view group;
	for (const ActionDef &def : registry()) {
		if (emitted != def.scope) {
			emitted = def.scope;
			group = {};
			out += std::format("\n[{}]\n", section_name(def.scope));
		}
		if (def.group != group) {
			group = def.group;
			out += std::format("# {}\n", group);
		}
		std::string keys;
		for (Chord chord : def.defaults) {
			if (!keys.empty()) {
				keys += ", ";
			}
			keys += chord_text(chord);
		}
		out += std::format("{} = {}\n", def.name, keys.empty() ? "none" : keys);
	}
	out += "\n[commands]\n"
		"# A chord here runs its command through /bin/sh -c, started from\n"
		"# the overview like the terminal action. Example:\n"
		"# Super+B = firefox\n";
	out += "\n[gestures]\n"
		"# swipes = three-finger-hops | four-finger-hops | off\n"
		"swipes = three-finger-hops\n"
		"# pinch-scale = on | off\n"
		"pinch-scale = on\n"
		"# invert = on | off: on reverses scrolling and the swipe hops\n"
		"invert = off\n";
	out += std::format("\n[session]\n"
		"# lock-idle = minutes of idleness before the screen locks, or off\n"
		"lock-idle = {}\n"
		"# night-light = on | off, its temperature {} to {} K\n"
		"night-light = off\n"
		"night-light-temperature = {}\n",
		default_lock_idle_minutes, night_light_min_k, night_light_max_k,
		default_night_light_k);
	return out;
}

// -------------------------------------------------------------- edits

File::File(std::string_view text)
	: parsed_(parse(text)), resolved_(resolve(parsed_)), text_(text) {
	std::size_t start = 0;
	for (std::size_t nl = text.find('\n'); nl != std::string_view::npos;
			nl = text.find('\n', start)) {
		lines_.emplace_back(text.substr(start, nl - start));
		start = nl + 1;
	}
	trailing_newline_ = !text.empty() && start == text.size();
	if (start < text.size()) {
		lines_.emplace_back(text.substr(start));
	}
}

int File::section_header(std::string_view section) const {
	for (std::size_t i = 0; i < lines_.size(); i++) {
		if (trim(lines_[i]) == std::format("[{}]", section)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

std::vector<int> File::entry_lines(std::string_view section,
		std::string_view name) const {
	std::vector<int> out;
	int header = section_header(section);
	if (header < 0) {
		return out;
	}
	for (std::size_t i = header + 1; i < lines_.size() && !is_header_line(lines_[i]);
			i++) {
		std::string_view n = line_name(lines_[i]);
		if (!n.empty() && (name.empty() || n == name)) {
			out.push_back(static_cast<int>(i));
		}
	}
	return out;
}

int File::append_after(std::string_view section) const {
	int header = section_header(section);
	if (header < 0) {
		return static_cast<int>(lines_.size()); // the section is missing
	}
	int last = header;
	for (std::size_t i = header + 1; i < lines_.size() && !is_header_line(lines_[i]);
			i++) {
		if (!trim(lines_[i]).empty()) {
			last = static_cast<int>(i);
		}
	}
	return last;
}

void File::write_line(int index, std::string_view name, std::string_view value,
		bool del_empty) {
	if (value.empty() && del_empty) {
		lines_.erase(lines_.begin() + index);
		return;
	}
	lines_[static_cast<std::size_t>(index)] =
		std::format("{} = {}", name, value.empty() ? "none" : value);
}

void File::append_entry(std::string_view section, std::string_view name,
		std::string_view value) {
	std::string line = std::format("{} = {}", name, value.empty() ? "none" : value);
	if (section_header(section) < 0) {
		// The section is missing: it is appended at the end of the file.
		if (!lines_.empty() && !trim(lines_.back()).empty()) {
			lines_.push_back("");
		}
		lines_.push_back(std::format("[{}]", section));
		lines_.push_back(std::move(line));
		return;
	}
	lines_.insert(lines_.begin() + append_after(section) + 1, std::move(line));
}

void File::rejoin() {
	text_.clear();
	for (std::size_t i = 0; i < lines_.size(); i++) {
		if (i > 0) {
			text_ += '\n';
		}
		text_ += lines_[i];
	}
	if (trailing_newline_ && !lines_.empty()) {
		text_ += '\n';
	}
}

void File::take_from_actions(const std::vector<Chord> &chords, Scope scope,
		std::string_view skip_action, std::vector<Moved> &moved) {
	const std::vector<ActionDef> &reg = registry();
	for (std::size_t i = 0; i < reg.size(); i++) {
		const ActionDef &def = reg[i];
		if (def.scope != scope || def.name == skip_action) {
			continue;
		}
		std::vector<Chord> taken;
		for (Chord chord : chords) {
			if (std::find(resolved_.keys[i].begin(), resolved_.keys[i].end(), chord)
					!= resolved_.keys[i].end()) {
				taken.push_back(chord);
			}
		}
		if (taken.empty()) {
			continue;
		}
		moved.push_back({.scope = scope, .action = std::string{def.name},
			.keys = taken});
		std::vector<int> lines = entry_lines(section_name(scope), def.name);
		if (lines.empty()) {
			continue; // it held them only by default: nothing to rewrite
		}
		std::vector<Chord> keep;
		for (std::string_view token : split(line_value(lines_[lines.front()]), ',')) {
			auto chord = parse_chord(token, scope);
			if (chord.has_value()
					&& std::find(taken.begin(), taken.end(), *chord) == taken.end()) {
				keep.push_back(*chord);
			}
		}
		std::string value;
		for (Chord chord : keep) {
			if (!value.empty()) {
				value += ", ";
			}
			value += chord_text(chord);
		}
		for (std::size_t k = lines.size(); k > 1; k--) {
			lines_.erase(lines_.begin() + lines[k - 1]);
		}
		write_line(lines.front(), def.name, value, true);
	}
}

void File::take_from_commands(const std::vector<Chord> &chords,
		std::vector<Moved> &moved) {
	std::vector<int> doomed;
	for (const Command &command : resolved_.commands) {
		if (std::find(chords.begin(), chords.end(), command.chord) == chords.end()) {
			continue;
		}
		moved.push_back({.scope = Scope::global, .action = {},
			.keys = {command.chord}, .command = command.command});
		for (int index : entry_lines("commands")) {
			auto chord = parse_chord(line_name(lines_[static_cast<std::size_t>(index)]),
				Scope::global);
			if (chord.has_value() && *chord == command.chord) {
				doomed.push_back(index);
			}
		}
	}
	std::sort(doomed.begin(), doomed.end(), std::greater{});
	for (int index : doomed) {
		lines_.erase(lines_.begin() + index);
	}
}

std::expected<Edit, std::string> File::set_action(Scope scope,
		std::string_view action, const std::optional<std::vector<Chord>> &keys) {
	const ActionDef *def = find_action(action, scope);
	if (def == nullptr) {
		return std::unexpected(std::format("unknown action '{}'", action));
	}
	// What the line gets: the given keys, or the action's defaults (`keys`
	// null is the reset: its defaults written into its line).
	std::vector<Chord> write;
	if (keys.has_value()) {
		for (std::size_t i = 0; i < keys->size(); i++) {
			for (std::size_t j = i + 1; j < keys->size(); j++) {
				if ((*keys)[i] == (*keys)[j]) {
					return std::unexpected(std::format("chord '{}' given twice",
						chord_text((*keys)[i])));
				}
			}
		}
		write = *keys;
	} else {
		write = def->defaults;
	}
	std::string value;
	for (Chord chord : write) {
		if (!value.empty()) {
			value += ", ";
		}
		value += chord_text(chord);
	}
	std::vector<int> lines = entry_lines(section_name(scope), action);
	if (lines.empty()) {
		append_entry(section_name(scope), action, value);
	} else {
		for (std::size_t k = lines.size(); k > 1; k--) {
			lines_.erase(lines_.begin() + lines[k - 1]);
		}
		write_line(lines.front(), action, value, false);
	}
	std::vector<Moved> moved;
	take_from_actions(write, scope, action, moved);
	take_from_commands(write, moved);
	rejoin();
	return Edit{text_, std::move(moved)};
}

std::expected<Edit, std::string> File::set_commands(
		const std::vector<Command> &commands) {
	for (std::size_t i = 0; i < commands.size(); i++) {
		for (std::size_t j = i + 1; j < commands.size(); j++) {
			if (commands[i].chord == commands[j].chord) {
				return std::unexpected(std::format("chord '{}' given twice",
					chord_text(commands[i].chord)));
			}
		}
	}
	std::vector<Chord> chords;
	for (const Command &command : commands) {
		chords.push_back(command.chord);
	}
	std::vector<Moved> moved;
	take_from_actions(chords, Scope::global, {}, moved);
	// The section's entries are replaced; its comments stay.
	std::vector<int> lines = entry_lines("commands");
	for (std::size_t k = lines.size(); k > 0; k--) {
		lines_.erase(lines_.begin() + lines[k - 1]);
	}
	if (section_header("commands") < 0) {
		if (!lines_.empty() && !trim(lines_.back()).empty()) {
			lines_.push_back("");
		}
		lines_.push_back("[commands]");
	}
	int at = append_after("commands");
	for (const Command &command : commands) {
		lines_.insert(lines_.begin() + at + 1,
			std::format("{} = {}", chord_text(command.chord), command.command));
		at++;
	}
	rejoin();
	return Edit{text_, std::move(moved)};
}

std::expected<Edit, std::string> File::set_swipes(SwipePreset preset) {
	std::string value{swipes_text(preset)};
	std::vector<int> lines = entry_lines("gestures", "swipes");
	if (lines.empty()) {
		append_entry("gestures", "swipes", value);
	} else {
		for (std::size_t k = lines.size(); k > 1; k--) {
			lines_.erase(lines_.begin() + lines[k - 1]);
		}
		write_line(lines.front(), "swipes", value, false);
	}
	rejoin();
	return Edit{text_, {}};
}

std::expected<Edit, std::string> File::set_session(std::string_view name,
		std::string_view value) {
	std::vector<int> lines = entry_lines("session", name);
	if (lines.empty()) {
		append_entry("session", name, std::string{value});
	} else {
		for (std::size_t k = lines.size(); k > 1; k--) {
			lines_.erase(lines_.begin() + lines[k - 1]);
		}
		write_line(lines.front(), name, std::string{value}, false);
	}
	rejoin();
	return Edit{text_, {}};
}

// The one shape every on/off line of [gestures] is written in: the first
// line that sets it is rewritten, any later duplicate dropped, and a
// setting the file does not name is appended to its section.
std::expected<Edit, std::string> File::set_gesture_switch(
		std::string_view name, bool on) {
	std::string value = on ? "on" : "off";
	std::vector<int> lines = entry_lines("gestures", name);
	if (lines.empty()) {
		append_entry("gestures", name, value);
	} else {
		for (std::size_t k = lines.size(); k > 1; k--) {
			lines_.erase(lines_.begin() + lines[k - 1]);
		}
		write_line(lines.front(), name, value, false);
	}
	rejoin();
	return Edit{text_, {}};
}

std::expected<Edit, std::string> File::set_pinch_scale(bool on) {
	return set_gesture_switch("pinch-scale", on);
}

std::expected<Edit, std::string> File::set_invert(bool on) {
	return set_gesture_switch("invert", on);
}

} // namespace settings
