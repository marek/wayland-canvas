// settings.cpp: canvas.conf — the action registry, the settings table's
// lookups, the file parse with an error per bad line, resolution by the
// precedence rules and the first-run template. Pure: no wlroots, no Server.

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/chord.h"
#include "core/settings.h"

namespace settings {

// The registry: defaults are written as chord text and parsed once; a default
// that fails to parse is dropped (the template test catches that).
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

const ActionDef *find_action(std::string_view name, Scope scope) {
	for (const ActionDef &def : registry()) {
		if (def.scope == scope && def.name == name) {
			return &def;
		}
	}
	return nullptr;
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

std::string_view section_name(Scope scope) {
	return scope == Scope::global ? "shortcuts" : "overview";
}

namespace {

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

} // namespace settings
