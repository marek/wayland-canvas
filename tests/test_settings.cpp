// Unit tests for src/core/settings.cpp: chord parse, format and validate;
// canvas.conf parse with an error per bad line; resolution by the precedence
// rules; the first-run template; the in-place edits settings.set makes.
// Run via `meson test` (test 'settings').

#include <cstdio>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "core/settings.h"

namespace {

int failures = 0;

void check(bool condition, const char *what,
	std::source_location location = std::source_location::current()) {
	if (!condition) {
		++failures;
		std::printf("FAIL %s:%u: %s\n", location.file_name(),
			static_cast<unsigned>(location.line()), what);
	}
}

void check(bool condition, const std::string &what,
	std::source_location location = std::source_location::current()) {
	check(condition, what.c_str(), location);
}

settings::Chord chord(std::string_view text,
	settings::Scope scope = settings::Scope::global) {
	auto parsed = settings::parse_chord(text, scope);
	check(parsed.has_value(),
		std::string{"chord '"} + std::string{text} + "' should parse");
	return parsed.value_or(settings::Chord{});
}

std::vector<settings::Chord> chords(std::initializer_list<std::string_view> texts,
	settings::Scope scope = settings::Scope::global) {
	std::vector<settings::Chord> out;
	for (std::string_view text : texts) {
		out.push_back(chord(text, scope));
	}
	return out;
}

// The resolved keys of `name` in `scope` as chord text.
std::vector<std::string> keys_of(const settings::Resolved &resolved,
		settings::Scope scope, std::string_view name) {
	std::size_t index = 0;
	for (const settings::ActionDef &def : settings::registry()) {
		if (def.scope == scope && def.name == name) {
			break;
		}
		index++;
	}
	std::vector<std::string> out;
	for (settings::Chord c : resolved.keys[index]) {
		out.push_back(settings::chord_text(c));
	}
	return out;
}

bool error_at(const settings::Parsed &parsed, int line, std::string_view text) {
	for (const settings::Error &error : parsed.errors) {
		if (error.line == line && error.text == text) {
			return true;
		}
	}
	return false;
}

bool error_on_line(const settings::Parsed &parsed, int line) {
	for (const settings::Error &error : parsed.errors) {
		if (error.line == line) {
			return true;
		}
	}
	return false;
}

void chords_parse_and_format() {
	// Global: every chord has Super, Ctrl or Alt, or a key that types nothing.
	for (std::string_view text : {
			"Super+Q", "Super+Return", "Super+Shift+M", "Ctrl+Alt+BackSpace",
			"Alt+F4", "Super+Ctrl+Shift+Left", "Super+Alt+Down", "Super+Space",
			"Shift+Print", "Ctrl+Shift+Print", "Alt+Print", "Print",
			"Super+Shift+3", "Super+0", "Super+Tab", "Super+W", "Super+F",
			"Super+Left", "Super+Up", "Super+M", "Super+T", "Super+Ctrl+D",
			"Super+Ctrl+F4", "Super+Shift+E", "Super+Shift+5", "Super+Shift+4",
			"Super+Ctrl+Shift+3", "F5", "Insert", "Pause", "Scroll_Lock",
			"XF86AudioPlay", "Super", "Super Super",
		}) {
		check(settings::parse_chord(text, settings::Scope::global).has_value(),
			std::string{"chord '"} + std::string{text} + "' parses globally");
		check(settings::chord_text(chord(text)) == text,
			std::string{"chord '"} + std::string{text} + "' round trips");
	}
	// The overview takes any chord, bare typing keys included.
	for (std::string_view text : {
			"BackSpace", "Delete", "Escape", "KP_Enter", "Plus", "Minus",
			"Equal", "Space", "3", "Return", "T", "Left",
		}) {
		check(settings::parse_chord(text, settings::Scope::overview).has_value(),
			std::string{"chord '"} + std::string{text} + "' parses in the overview");
		check(settings::chord_text(chord(text, settings::Scope::overview)) == text,
			std::string{"chord '"} + std::string{text} + "' round trips");
	}
}

void chords_case_insensitive() {
	check(settings::chord_text(chord("super+q")) == "Super+Q",
		"lower-case input parses and formats canonically");
	check(settings::chord_text(chord("SUPER+SHIFT+M")) == "Super+Shift+M",
		"upper-case input parses and formats canonically");
	check(settings::chord_text(chord("super+return")) == "Super+Return",
		"a key name parses case-insensitively");
	check(settings::chord_text(chord("backspace", settings::Scope::overview))
			== "BackSpace",
		"a multi-word key name parses case-insensitively");
	check(settings::chord_text(chord("kp_enter", settings::Scope::overview))
			== "KP_Enter",
		"KP_Enter round trips");
	check(settings::chord_text(chord("print")) == "Print", "Print round trips");
	check(settings::chord_text(chord("f4")) == "F4", "F4 round trips");
	check(settings::chord_text(chord("3", settings::Scope::overview)) == "3",
		"a digit key round trips");
}

void canonical_modifier_order() {
	check(settings::chord_text(chord("shift+ctrl+super+q")) == "Super+Ctrl+Shift+Q",
		"modifiers are written Super, Ctrl, Alt, Shift, whatever the input order");
	check(settings::chord_text(chord("alt+shift+q")) == "Alt+Shift+Q",
		"two modifiers sort canonically too");
	check(settings::chord_text(chord("SHIFT+ALT+q")) == "Alt+Shift+Q",
		"canonical order holds for mixed case");
}

void the_super_tap() {
	settings::Chord tap = chord("Super");
	check(tap.tap(), "a lone Super parses as the tap");
	check(settings::chord_text(tap) == "Super", "the tap formats as `Super`");
	check(settings::parse_chord("super", settings::Scope::overview).has_value()
			&& settings::parse_chord("super", settings::Scope::global).has_value(),
		"the tap parses in both scopes");
	check(!settings::parse_chord("Ctrl", settings::Scope::global).has_value(),
		"a lone Ctrl is an error");
	check(!settings::parse_chord("Ctrl", settings::Scope::overview).has_value(),
		"a lone Ctrl is an error in the overview too");
	check(!settings::parse_chord("Super+Ctrl", settings::Scope::overview).has_value(),
		"a modifier as the key of a chord is an error");
}

void the_super_double_tap() {
	settings::Chord twice = chord("Super Super");
	check(twice.tap() && twice.twice, "two Supers parse as the double tap");
	check(twice != chord("Super"), "the double tap is not the lone tap");
	check(settings::chord_text(chord("super  SUPER")) == "Super Super",
		"the double tap reads in any case and spacing, written `Super Super`");
	check(settings::parse_chord("Super Super", settings::Scope::overview).has_value(),
		"the double tap parses in the overview too");
	check(!settings::parse_chord("Super Super Super", settings::Scope::global).has_value()
			&& !settings::parse_chord("Super Ctrl", settings::Scope::global).has_value()
			&& !settings::parse_chord("Super Super+K", settings::Scope::global).has_value(),
		"only two Supers make the double tap");
	settings::Resolved taken = settings::resolve(settings::parse(
		"[shortcuts]\nlauncher = Super Super\n"));
	check(keys_of(taken, settings::Scope::global, "launcher")
				== std::vector<std::string>{"Super Super"}
			&& keys_of(taken, settings::Scope::global, "pan-back").empty(),
		"a file giving the double tap to another action takes it from pan-back");
	settings::Resolved apart = settings::resolve(settings::parse(
		"[shortcuts]\nlauncher = Super\n"));
	check(keys_of(apart, settings::Scope::global, "launcher")
				== std::vector<std::string>{"Super"}
			&& keys_of(apart, settings::Scope::global, "pan-back")
				== std::vector<std::string>{"Super Super"},
		"the lone tap and the double tap are held apart");
}

void validation_per_scope() {
	auto global_error = [](std::string_view text) {
		auto parsed = settings::parse_chord(text, settings::Scope::global);
		return parsed.has_value() ? std::string{} : parsed.error();
	};
	check(global_error("A") == "A needs Super, Ctrl or Alt",
		"a bare typing key is rejected with the reason");
	check(global_error("Shift+A") == "A needs Super, Ctrl or Alt",
		"Shift alone does not make a chord valid");
	check(global_error("x") == "X needs Super, Ctrl or Alt",
		"a lower-case key is named by its upper-case form");
	check(global_error("Foo?").starts_with("unknown key"),
		"an unknown key is an error");
	check(settings::parse_chord("F5", settings::Scope::global).has_value(),
		"a function key needs no modifier");
	check(settings::parse_chord("Print", settings::Scope::global).has_value(),
		"Print types no character");
	check(settings::parse_chord("XF86Terminal", settings::Scope::global).has_value(),
		"an XF86 key types no character");
	check(settings::parse_chord("Super+X", settings::Scope::global).has_value(),
		"Super makes any key valid");
	check(settings::parse_chord("A", settings::Scope::overview).has_value(),
		"the overview scope takes any chord");
	check(settings::parse_chord("Left", settings::Scope::overview).has_value(),
		"a bare arrow is valid in the overview");
}

void registry_defaults() {
	check(keys_of(settings::resolve(settings::parse("")), settings::Scope::global, "launcher")
			== std::vector<std::string>{"Super+Space"},
		"an empty file resolves to the launcher's default");
	check(keys_of(settings::resolve(settings::parse("")), settings::Scope::global, "close")
			== std::vector<std::string>({"Super+Q", "Alt+F4"}),
		"close holds two default chords");
	check(keys_of(settings::resolve(settings::parse("")), settings::Scope::overview, "zoom-in")
			== std::vector<std::string>({"Plus", "Equal"}),
		"the overview's zoom-in defaults parse");
	check(keys_of(settings::resolve(settings::parse("")), settings::Scope::overview, "leave")
			== std::vector<std::string>({"Super+Tab", "Super+W"}),
		"leaving the overview has its own chords");
	const settings::ActionDef *tap_holder = nullptr;
	for (const settings::ActionDef &def : settings::registry()) {
		for (settings::Chord c : def.defaults) {
			bool lone = c.tap() && !c.twice;
			check(!lone, std::string{"no default is the lone tap ("}
				+ std::string{def.name} + ")");
			if (lone) {
				tap_holder = &def;
			}
		}
	}
	check(tap_holder == nullptr, "the lone Super tap is no action's default");
	check(keys_of(settings::resolve(settings::parse("")), settings::Scope::global, "pan-back")
			== std::vector<std::string>{"Super Super"},
		"the double tap goes back from a look away by default");
}

void precedence_file_beats_default() {
	settings::Resolved resolved =
		settings::resolve(settings::parse("[shortcuts]\nlauncher = Super+K\n"));
	check(keys_of(resolved, settings::Scope::global, "launcher") == std::vector<std::string>{"Super+K"},
		"a file line replaces an action's keys wholly");
	check(keys_of(resolved, settings::Scope::global, "terminal") == std::vector<std::string>{"Super+Return"},
		"an untouched action keeps its defaults");
}

void precedence_first_line_wins() {
	settings::Parsed parsed = settings::parse(
		"[shortcuts]\nterminal = Super+Return, Super+T\ntiling = Super+T\n");
	check(parsed.errors.size() == 1 && parsed.errors[0].line == 3,
		"a chord on a later line of the scope is an error on that line");
	check(keys_of(settings::resolve(parsed), settings::Scope::global, "terminal")
			== std::vector<std::string>({"Super+Return", "Super+T"}),
		"the first line keeps the chord");
	check(keys_of(settings::resolve(parsed), settings::Scope::global, "tiling").empty(),
		"the later line loses the chord (the rest of that line stands)");
}

void precedence_none() {
	settings::Resolved resolved =
		settings::resolve(settings::parse("[shortcuts]\nlauncher = none\n"));
	check(keys_of(resolved, settings::Scope::global, "launcher").empty(), "`none` leaves the action keyless");
	check(keys_of(resolved, settings::Scope::global, "terminal") == std::vector<std::string>{"Super+Return"},
		"`none` touches only its own action");
}

void precedence_taken_from_default_holders() {
	settings::Resolved resolved = settings::resolve(settings::parse(
		"[commands]\nSuper+Space = firefox\n"));
	check(keys_of(resolved, settings::Scope::global, "launcher").empty(),
		"a chord given to a command is taken from the action holding it by default");
	check(resolved.commands.size() == 1
			&& settings::chord_text(resolved.commands[0].chord) == "Super+Space"
			&& resolved.commands[0].command == "firefox",
		"the command itself stands");
	check(keys_of(resolved, settings::Scope::global, "terminal") == std::vector<std::string>{"Super+Return"},
		"other defaults are untouched");
}

void errors_carry_line_numbers() {
	settings::Parsed parsed = settings::parse(
		"# a comment line\n"
		"launcher = Super+Q\n"
		"[shortcuts]\n"
		"lancher = Super+P\n"
		"terminal = Supr+T\n"
		"[whatever]\n"
		"x = y\n"
		"[gestures]\n"
		"swipes = sometimes\n"
		"pinch-scale = maybe\n"
		"waggle = on\n");
	check(error_at(parsed, 2, "a line before any section"),
		"a line before any section is an error with its number");
	check(error_at(parsed, 4, "unknown action 'lancher'"),
		"an unknown action is an error with its number");
	check(error_on_line(parsed, 5), "a bad chord is an error with its number");
	check(error_at(parsed, 6, "unknown section 'whatever'"),
		"an unknown section is an error with its number");
	check(!error_on_line(parsed, 7),
		"an unknown section's lines are ignored, not errors");
	check(error_on_line(parsed, 9) && error_on_line(parsed, 10) && error_on_line(parsed, 11),
		"bad gesture values are errors with their numbers");
	// Everything else still applies.
	check(keys_of(settings::resolve(parsed), settings::Scope::global, "launcher")
			== std::vector<std::string>{"Super+Space"},
		"the good lines apply around the errors");
}

// [gestures] directions: each kind of scroll and swipe turns on its own
// (docs/intents/input/gestures.md).
void gesture_directions() {
	settings::Resolved defaults = settings::resolve(settings::parse(""));
	for (const settings::Direction &direction : settings::directions) {
		check(!(defaults.*direction.turned),
			std::format("{} defaults to {}", direction.key, direction.way));
	}
	// Each key turns its own direction and no other.
	for (const settings::Direction &direction : settings::directions) {
		settings::Parsed parsed = settings::parse(
			std::format("[gestures]\n{} = {}\n", direction.key, direction.other));
		settings::Resolved resolved = settings::resolve(parsed);
		bool only_it = parsed.errors.empty();
		for (const settings::Direction &other : settings::directions) {
			only_it = only_it && resolved.*other.turned == (&other == &direction);
		}
		check(only_it, std::format("{} = {} turns that direction alone", direction.key,
			direction.other));
	}
	settings::Parsed bad = settings::parse("[gestures]\nhop-swipe = sideways\n");
	check(error_at(bad, 2, "hop-swipe must be toward or away"),
		"a value that is neither way is an error naming both, with its line");
	check(!settings::resolve(bad).hops_away, "the bad line leaves the default");

	settings::File file{"[gestures]\npan = natural\npan = natural\n"};
	auto edit = file.set_direction("pan", "traditional");
	check(edit.has_value() && edit->text == "[gestures]\npan = traditional\n",
		"set_direction rewrites the first line and drops the duplicate");
	check(edit.has_value() && settings::File{edit->text}.resolved().pan_traditional,
		"the written way resolves");
	check(!file.set_direction("pan", "sideways").has_value()
			&& !file.set_direction("wheel", "natural").has_value(),
		"an unknown way or direction is an error");
}

void lock_idle_has_a_range() {
	settings::Parsed top = settings::parse(
		std::format("[session]\nlock-idle = {}\n", settings::max_lock_idle_minutes));
	check(!error_on_line(top, 2) && settings::resolve(top).lock_idle_minutes
			== settings::max_lock_idle_minutes,
		"the largest idle time the timer holds is accepted");
	for (std::string_view past : {"35792", "4294967301", "0", "-3"}) {
		settings::Parsed parsed = settings::parse(
			std::format("[session]\nlock-idle = {}\n", past));
		check(error_on_line(parsed, 2) && settings::resolve(parsed).lock_idle_minutes
				== settings::default_lock_idle_minutes,
			std::format("lock-idle = {} is an error and the default stays", past));
	}
	check(!error_on_line(settings::parse("[session]\nlock-idle = off\n"), 2),
		"lock-idle = off is accepted");
}

void template_parses_back() {
	std::string text = settings::template_text();
	settings::Parsed parsed = settings::parse(text);
	check(parsed.errors.empty(), "the template parses without errors");
	settings::Resolved from_template = settings::resolve(parsed);
	settings::Resolved defaults = settings::resolve(settings::parse(""));
	check(from_template == defaults,
		"the template resolves to exactly the defaults");
	check(text.find("[commands]\n") != std::string::npos
			&& text.find("# Super+B = firefox") != std::string::npos,
		"the template holds an empty [commands] with a commented example");
	bool directions_written = true;
	for (const settings::Direction &direction : settings::directions) {
		directions_written = directions_written
			&& text.find(std::format("\n{} = {}\n", direction.key, direction.way))
				!= std::string::npos;
	}
	check(text.find("[gestures]\n") != std::string::npos
			&& text.find("swipes = three-finger-hops") != std::string::npos
			&& text.find("pinch-scale = on") != std::string::npos && directions_written,
		"the template holds [gestures] at its defaults, every direction too");
	check(text.find("# Apps\n") != std::string::npos
			&& text.find("# Windows\n") != std::string::npos
			&& text.find("# In the overview\n") != std::string::npos,
		"the template names the groups");
}

void edit_keeps_every_other_byte() {
	std::string before =
		"# the header\n"
		"\n"
		"[shortcuts]\n"
		"# Apps first\n"
		"launcher=Super+Space\n"
		"terminal =   Super+Return\n"
		"\n"
		"[commands]\n"
		"# keep me\n"
		"Super+B = firefox # a comment after the value\n";
	settings::File file{before};
	auto edit = file.set_action(settings::Scope::global, "launcher",
		std::optional{chords({"Super+K"})});
	check(edit.has_value(), "set_action on an existing line succeeds");
	std::string after = edit->text;
	// Line by line: only the launcher line may differ.
	std::vector<std::string> before_lines, after_lines;
	for (std::size_t i = 0, start = 0; i <= before.size(); i++) {
		if (i == before.size() || before[i] == '\n') {
			before_lines.push_back(before.substr(start, i - start));
			start = i + 1;
		}
	}
	for (std::size_t i = 0, start = 0; i <= after.size(); i++) {
		if (i == after.size() || after[i] == '\n') {
			after_lines.push_back(after.substr(start, i - start));
			start = i + 1;
		}
	}
	check(before_lines.size() == after_lines.size(),
		"the edit changed no line count");
	if (before_lines.size() == after_lines.size()) {
		int changed = 0;
		for (std::size_t i = 0; i < before_lines.size(); i++) {
			if (before_lines[i] != after_lines[i]) {
				changed++;
				check(after_lines[i] == "launcher = Super+K",
					"the one changed line is the rewritten entry");
			}
		}
		check(changed == 1, "exactly one line changed");
	}
	// And the new file parses to what was set.
	settings::File reloaded{after};
	check(keys_of(reloaded.resolved(), settings::Scope::global, "launcher")
			== std::vector<std::string>{"Super+K"},
		"the edited file resolves to the new keys");
	check(keys_of(reloaded.resolved(), settings::Scope::global, "terminal")
			== std::vector<std::string>{"Super+Return"},
		"the odd spacing of an untouched line still parses");
	check(reloaded.parsed().commands.size() == 1
			&& reloaded.parsed().commands[0].value
				== "firefox # a comment after the value",
		"'#' inside a value is literal");
}

void edit_appends_into_missing_section() {
	std::string before = "[shortcuts]\nlauncher = Super+Space\n";
	settings::File file{before};
	auto edit = file.set_swipes(settings::SwipePreset::four_finger_hops);
	check(edit.has_value(), "set_swipes succeeds");
	check(edit->text == "[shortcuts]\nlauncher = Super+Space\n"
			"\n[gestures]\nswipes = four-finger-hops\n",
		"a missing section is appended at the end of the file");
	settings::File reloaded{edit->text};
	check(reloaded.resolved().swipes == settings::SwipePreset::four_finger_hops,
		"the appended value resolves");

	settings::File no_overview{"[shortcuts]\nlauncher = none\n"};
	auto overview_edit = no_overview.set_action(settings::Scope::overview, "go",
		std::optional{chords({"Return"}, settings::Scope::overview)});
	check(overview_edit.has_value()
			&& overview_edit->text.find("[overview]\ngo = Return\n")
				!= std::string::npos,
		"a missing [overview] section is appended too");
}

void edit_reports_moved() {
	// From a file without the launcher's line: taking Super+Space for the
	// terminal moves it off the launcher, which holds it only by default
	// (no line rewritten).
	settings::File file{"[shortcuts]\nterminal = Super+Return\n"};
	auto edit = file.set_action(settings::Scope::global, "terminal",
		std::optional{chords({"Super+Space"})});
	check(edit.has_value() && edit->moved.size() == 1
			&& edit->moved[0].action == "launcher"
			&& settings::chord_text(edit->moved[0].keys[0]) == "Super+Space",
		"a chord taken from a default holder is reported moved");
	check(edit->text == "[shortcuts]\nterminal = Super+Space\n",
		"a default holder has no line to rewrite");

	// From a file line: the loser's line is rewritten without the chord.
	std::string with_lines =
		"[shortcuts]\nlauncher = Super+Space, Super+L\nterminal = Super+Return\n";
	settings::File lined{with_lines};
	auto lined_edit = lined.set_action(settings::Scope::global, "terminal",
		std::optional{chords({"Super+Space"})});
	check(lined_edit.has_value() && lined_edit->moved.size() == 1
			&& lined_edit->moved[0].action == "launcher",
		"the loser action is reported");
	check(lined_edit->text ==
			"[shortcuts]\nlauncher = Super+L\nterminal = Super+Space\n",
		"the loser's line keeps its other chords, the winner's is replaced");

	// A command losing its chord: its line goes.
	std::string with_command =
		"[shortcuts]\nlauncher = Super+Space\n\n[commands]\nSuper+K = foot\n";
	settings::File commanded{with_command};
	auto commanded_edit = commanded.set_action(settings::Scope::global, "terminal",
		std::optional{chords({"Super+K"})});
	check(commanded_edit.has_value() && commanded_edit->moved.size() == 1
			&& commanded_edit->moved[0].action.empty()
			&& commanded_edit->moved[0].command == "foot",
		"a losing command is reported with its command text");
	check(commanded_edit->text ==
			"[shortcuts]\nlauncher = Super+Space\nterminal = Super+K\n"
			"\n[commands]\n",
		"the losing command's line is removed");

	// set_commands takes from an action with a file line.
	settings::File both{
		"[shortcuts]\nterminal = Super+Return\n\n[commands]\n"};
	auto commands_edit = both.set_commands(
		{{chord("Super+Return"), "foot"}});
	check(commands_edit.has_value() && commands_edit->moved.size() == 1
			&& commands_edit->moved[0].action == "terminal",
		"a command taking an action's chord reports the action");
	check(commands_edit->text == "[shortcuts]\n\n[commands]\nSuper+Return = foot\n",
		"the action's emptied line went, the command was written");
}

void edit_back_to_defaults() {
	std::string before = "[shortcuts]\nlauncher = Super+K\n";
	settings::File file{before};
	auto edit = file.set_action(settings::Scope::global, "launcher", {});
	check(edit.has_value()
			&& edit->text == "[shortcuts]\nlauncher = Super+Space\n",
		"a null keys list writes the action's defaults into its line");
	settings::File reloaded{edit->text};
	check(keys_of(reloaded.resolved(), settings::Scope::global, "launcher")
			== std::vector<std::string>{"Super+Space"},
		"the written defaults resolve as the defaults");
}

void edit_errors() {
	settings::File file{settings::template_text()};
	auto unknown = file.set_action(settings::Scope::global, "lancher",
		std::optional{chords({"Super+K"})});
	check(!unknown.has_value() && unknown.error() == "unknown action 'lancher'",
		"an unknown action is an error");
	auto bad_scope = file.set_action(settings::Scope::overview, "launcher",
		std::optional{chords({"Super+K"})});
	check(!bad_scope.has_value(),
		"a global action is unknown in the overview scope");
	auto dup = file.set_action(settings::Scope::global, "launcher",
		std::optional{chords({"Super+K", "Super+K"})});
	check(!dup.has_value() && dup.error() == "chord 'Super+K' given twice",
		"a chord twice in the new keys is an error");
	auto dup_commands = file.set_commands(
		{{chord("Super+B"), "firefox"}, {chord("Super+B"), "foot"}});
	check(!dup_commands.has_value(),
		"a chord twice in the new commands is an error");
}

void overview_resolution() {
	settings::Resolved resolved = settings::resolve(settings::parse(
		"[overview]\ngo = KP_Enter\ntiling = none\n"));
	check(keys_of(resolved, settings::Scope::overview, "go") == std::vector<std::string>{"KP_Enter"},
		"an overview line replaces that action's overview keys");
	check(keys_of(resolved, settings::Scope::overview, "tiling").empty(),
		"`none` works in the overview too");
	check(keys_of(resolved, settings::Scope::overview, "back") == std::vector<std::string>{"Escape"},
		"untouched overview actions keep their defaults");
	check(keys_of(resolved, settings::Scope::global, "launcher") == std::vector<std::string>{"Super+Space"},
		"overview lines leave global actions alone");
}

} // namespace

int main() {
	chords_parse_and_format();
	chords_case_insensitive();
	canonical_modifier_order();
	the_super_tap();
	the_super_double_tap();
	validation_per_scope();
	registry_defaults();
	precedence_file_beats_default();
	precedence_first_line_wins();
	precedence_none();
	precedence_taken_from_default_holders();
	errors_carry_line_numbers();
	gesture_directions();
	template_parses_back();
	lock_idle_has_a_range();
	edit_keeps_every_other_byte();
	edit_appends_into_missing_section();
	edit_reports_moved();
	edit_back_to_defaults();
	edit_errors();
	overview_resolution();

	if (failures == 0) {
		std::printf("ALL PASSED\n");
		return 0;
	}
	std::printf("%d failure(s)\n", failures);
	return 1;
}
