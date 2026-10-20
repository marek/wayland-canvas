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
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/settings.h"

namespace settings {

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

namespace {

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

} // namespace

// The registry (the settings contract §3): [shortcuts] scope global first,
// then [overview] with group "In the overview". Defaults are written as
// chord text and parsed once; a default that fails to parse is dropped (and
// the template test catches that).
const std::vector<ActionDef> &registry() {
	static const std::vector<ActionDef> table = [] {
		std::vector<ActionDef> defs;
		auto add = [&](Scope scope, std::string_view group, std::string_view name,
				std::string_view label, std::initializer_list<const char *> keys,
				Reach reach = Reach::scope) {
			std::vector<Chord> chords;
			for (const char *key : keys) {
				if (auto chord = parse_chord(key, scope)) {
					chords.push_back(*chord);
				}
			}
			defs.push_back({scope, name, label, group, std::move(chords), reach});
		};
		add(Scope::global, "Apps", "launcher", "App launcher", {"Super+Space"},
			Reach::overview);
		add(Scope::global, "Apps", "terminal", "Terminal", {"Super+Return"},
			Reach::overview);
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
		// The capture chords are tried before all else.
		auto capture = [&](std::string_view name, std::string_view label,
				std::initializer_list<const char *> keys) {
			add(Scope::global, "Capture", name, label, keys, Reach::everywhere);
		};
		capture("shot-screen", "Screenshot", {"Super+Shift+3", "Shift+Print"});
		capture("shot-screen-copy", "Screenshot to the clipboard",
			{"Super+Ctrl+Shift+3", "Ctrl+Shift+Print"});
		capture("capture-pick", "Shot of a region or window",
			{"Super+Shift+4", "Print"});
		capture("capture-pick-copy", "Shot of a region or window to the clipboard",
			{"Super+Ctrl+Shift+4", "Ctrl+Print"});
		capture("capture-toolbar", "Capture toolbar", {"Super+Shift+5"});
		capture("capture-toolbar-copy", "Capture toolbar, to the clipboard",
			{"Super+Ctrl+Shift+5"});
		capture("record-stop", "Stop the recording", {"Alt+Print"});
		capture("record-stop-copy", "Stop the recording, to the clipboard",
			{"Ctrl+Alt+Print"});
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

int Field::read(const Resolved &resolved) const {
	return std::visit([&](auto held) { return static_cast<int>(resolved.*held); }, member);
}

void Field::write(Resolved &resolved, int value) const {
	std::visit([&](auto held) {
		using Held = std::remove_reference_t<decltype(resolved.*held)>;
		resolved.*held = static_cast<Held>(value);
	}, member);
}

Resolved::Resolved() {
	for (const Field &field : fields) {
		field.write(*this, field.fallback);
	}
}

const Field *find_field(std::string_view section, std::string_view key) {
	auto it = std::ranges::find_if(fields, [&](const Field &field) {
		return field.section == section && field.key == key;
	});
	return it != fields.end() ? &*it : nullptr;
}

const Field *find_param(std::string_view param) {
	auto it = std::ranges::find(fields, param, &Field::param);
	return it != fields.end() ? &*it : nullptr;
}

namespace {

bool in_bounds(const Field &field, long long number) {
	return number >= field.min && number <= field.max;
}

// `a, b or c`.
std::string join_or(std::span<const std::string_view> names) {
	std::string out;
	for (std::size_t i = 0; i < names.size(); i++) {
		if (i > 0) {
			out += i + 1 == names.size() ? " or " : ", ";
		}
		out += names[i];
	}
	return out;
}

} // namespace

std::optional<int> parse_value(const Field &field, std::string_view text) {
	if (field.kind != Kind::integer) {
		std::span<const std::string_view> names = field.values();
		auto it = std::ranges::find(names, text);
		if (it == names.end()) {
			return std::nullopt;
		}
		return static_cast<int>(it - names.begin());
	}
	if (field.off && text == "off") {
		return 0;
	}
	long long number = 0;
	auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), number);
	if (err != std::errc{} || end != text.data() + text.size() || !in_bounds(field, number)) {
		return std::nullopt;
	}
	return static_cast<int>(number);
}

std::string value_text(const Field &field, int value) {
	if (field.kind != Kind::integer) {
		return std::string{field.values()[static_cast<std::size_t>(value)]};
	}
	return value == 0 && field.off ? "off" : std::to_string(value);
}

bool allows(const Field &field, long long value) {
	if (field.kind != Kind::integer) {
		return value >= 0 && value < static_cast<long long>(field.values().size());
	}
	return in_bounds(field, value) || (field.off && value == 0);
}

std::string allowed(const Field &field) {
	switch (field.kind) {
	case Kind::choice:
		return join_or(field.values());
	case Kind::toggle:
		return "on or off";
	case Kind::integer:
		return std::format("{} to {} {}{}", field.min, field.max, field.unit,
			field.off ? " or off" : "");
	}
	return {};
}

std::string fields_help() {
	std::string out;
	std::string_view section;
	for (const Field &field : fields) {
		if (field.section != section) {
			section = field.section;
			out += std::format("  [{}]\n", section);
		}
		out += std::format("    {} = {}\n        {}; default {}\n", field.key,
			allowed(field), field.help, value_text(field, field.fallback));
	}
	return out;
}

std::string_view scope_name(Scope scope) {
	return scope == Scope::global ? "global" : "overview";
}

std::optional<Scope> parse_scope(std::string_view name) {
	for (Scope scope : {Scope::global, Scope::overview}) {
		if (name == scope_name(scope)) {
			return scope;
		}
	}
	return std::nullopt;
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

// The chords as the file writes them: `Super+Q, Alt+F4`.
std::string chords_text(const std::vector<Chord> &chords) {
	std::string out;
	for (Chord chord : chords) {
		if (!out.empty()) {
			out += ", ";
		}
		out += chord_text(chord);
	}
	return out;
}

// The error for the first chord `chords` holds twice.
std::expected<void, std::string> reject_repeats(const std::vector<Chord> &chords) {
	for (std::size_t i = 0; i < chords.size(); i++) {
		for (std::size_t j = i + 1; j < chords.size(); j++) {
			if (chords[i] == chords[j]) {
				return std::unexpected(std::format("chord '{}' given twice",
					chord_text(chords[i])));
			}
		}
	}
	return {};
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

bool is_setting_section(std::string_view name) {
	return std::ranges::any_of(fields, [&](const Field &field) { return field.section == name; });
}

// One `key = value` line of [gestures] or [session]: the setting, or an
// error saying what the field takes.
void parse_setting_line(Parsed &out, int line, std::string_view section,
		std::string_view key, std::string_view value) {
	const Field *field = find_field(section, key);
	if (field == nullptr) {
		out.errors.push_back({.line = line,
			.text = std::format("unknown setting '{}'", key)});
		return;
	}
	std::optional<int> parsed = parse_value(*field, value);
	if (!parsed.has_value()) {
		out.errors.push_back({.line = line,
			.text = std::format("{} must be {}", key, allowed(*field))});
		return;
	}
	out.settings.push_back({.line = line, .field = field, .value = *parsed});
}

} // namespace

// ------------------------------------------------------------- parsing

Parsed parse(std::string_view text) {
	Parsed out;
	enum class Section { none, shortcuts, overview, commands, setting, unknown };
	Section section = Section::none;
	std::string_view setting_section; // [gestures] or [session], while in one
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
			} else if (is_setting_section(name)) {
				section = Section::setting;
				setting_section = name;
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
		case Section::setting:
			parse_setting_line(out, line_no, setting_section, name, value);
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
	// A setting is its first line's.
	std::array<bool, fields.size()> seen{};
	for (const Setting &setting : parsed.settings) {
		std::size_t index = static_cast<std::size_t>(setting.field - fields.data());
		if (!seen[index]) {
			setting.field->write(out, setting.value);
			seen[index] = true;
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
		out += std::format("{} = {}\n", def.name,
			def.defaults.empty() ? "none" : chords_text(def.defaults));
	}
	out += "\n[commands]\n"
		"# A chord here runs its command through /bin/sh -c, started from\n"
		"# the overview like the terminal action. Example:\n"
		"# Super+B = firefox\n";
	std::string_view section;
	for (const Field &field : fields) {
		if (field.section != section) {
			section = field.section;
			out += std::format("\n[{}]\n", section);
		}
		out += std::format("# {}\n# {} = {}\n{} = {}\n", field.help, field.key,
			allowed(field), field.key, value_text(field, field.fallback));
	}
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

void File::erase_lines(const std::vector<int> &lines) {
	for (std::size_t k = lines.size(); k > 0; k--) {
		lines_.erase(lines_.begin() + lines[k - 1]);
	}
}

void File::ensure_section(std::string_view section) {
	if (section_header(section) >= 0) {
		return;
	}
	if (!lines_.empty() && !trim(lines_.back()).empty()) {
		lines_.push_back("");
	}
	lines_.push_back(std::format("[{}]", section));
}

void File::append_entry(std::string_view section, std::string_view name,
		std::string_view value) {
	ensure_section(section);
	lines_.insert(lines_.begin() + append_after(section) + 1,
		std::format("{} = {}", name, value.empty() ? "none" : value));
}

void File::write_entry(std::string_view section, std::string_view name,
		std::string_view value) {
	std::vector<int> lines = entry_lines(section, name);
	if (lines.empty()) {
		append_entry(section, name, value);
		return;
	}
	erase_lines(std::vector<int>(lines.begin() + 1, lines.end()));
	lines_[static_cast<std::size_t>(lines.front())] =
		std::format("{} = {}", name, value.empty() ? "none" : value);
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
		if (keep.empty()) {
			erase_lines(lines);
		} else {
			write_entry(section_name(scope), def.name, chords_text(keep));
		}
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
	std::ranges::sort(doomed);
	erase_lines(doomed);
}

std::expected<Edit, std::string> File::set_action(Scope scope,
		std::string_view action, const std::optional<std::vector<Chord>> &keys) {
	const ActionDef *def = find_action(action, scope);
	if (def == nullptr) {
		return std::unexpected(std::format("unknown action '{}'", action));
	}
	// What the line gets: the given keys, or the action's defaults (`keys`
	// null is the reset: its defaults written into its line).
	const std::vector<Chord> &write = keys.has_value() ? *keys : def->defaults;
	if (auto repeat = reject_repeats(write); !repeat.has_value()) {
		return std::unexpected(repeat.error());
	}
	write_entry(section_name(scope), action, chords_text(write));
	std::vector<Moved> moved;
	take_from_actions(write, scope, action, moved);
	take_from_commands(write, moved);
	rejoin();
	return Edit{text_, std::move(moved)};
}

std::expected<Edit, std::string> File::set_commands(
		const std::vector<Command> &commands) {
	std::vector<Chord> chords;
	for (const Command &command : commands) {
		chords.push_back(command.chord);
	}
	if (auto repeat = reject_repeats(chords); !repeat.has_value()) {
		return std::unexpected(repeat.error());
	}
	std::vector<Moved> moved;
	take_from_actions(chords, Scope::global, {}, moved);
	// The section's entries are replaced; its comments stay.
	erase_lines(entry_lines("commands"));
	ensure_section("commands");
	int at = append_after("commands");
	for (const Command &command : commands) {
		lines_.insert(lines_.begin() + at + 1,
			std::format("{} = {}", chord_text(command.chord), command.command));
		at++;
	}
	rejoin();
	return Edit{text_, std::move(moved)};
}

std::expected<Edit, std::string> File::set(const Field &field, long long value) {
	if (!allows(field, value)) {
		return std::unexpected(std::format("{} must be {}", field.key, allowed(field)));
	}
	write_entry(field.section, field.key, value_text(field, static_cast<int>(value)));
	rejoin();
	return Edit{text_, {}};
}

} // namespace settings
