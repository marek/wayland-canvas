#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/chord.h"

namespace settings {

// The scope as the control socket and the settings window name it: `global`
// or `overview`.
std::string_view scope_name(Scope scope);
// The scope a name names; nullopt for any other text.
std::optional<Scope> parse_scope(std::string_view name);
// The file section that holds `scope`'s actions: `shortcuts` or `overview`.
std::string_view section_name(Scope scope);

// Where an action's chords are tried, apart from its scope's own table: the
// global table serves every view but the overview, the overview table the
// overview. `overview` is a global action whose chords the overview takes
// too (the launcher, the terminal); `everywhere` is one whose chords are
// tried before all else, so they also work in the overview and while a
// capture UI holds the keyboard (the capture actions).
enum class Reach { scope, overview, everywhere };

// What the `swipes` setting chose. GestureOptions (input/gesture.h) carries it
// into the gesture code. The order is the order of that setting's names.
enum class SwipePreset { three_finger_hops, four_finger_hops, off };

// One action of the registry (docs/plans' settings contract §3), in registry
// order. `group` and `label` are only how the settings window names it:
// where its chords apply is `reach`.
struct ActionDef {
	Scope scope;
	std::string_view name;
	std::string_view label;
	std::string_view group;
	std::vector<Chord> defaults;
	Reach reach = Reach::scope;
};

// Every action in registry order: the [shortcuts] ones first, the [overview]
// ones last with group "In the overview".
const std::vector<ActionDef> &registry();
// The action `name` of `scope`, or nullptr.
const ActionDef *find_action(std::string_view name, Scope scope);

// One bad line of canvas.conf, with its 1-based line number.
struct Error {
	int line = 0;
	std::string text;
};

// A `name = value` line of [shortcuts], [overview] or [commands] that parsed
// cleanly, with its 1-based line number. `name` is the action name, or the
// chord text ([commands], where the chord names the line).
struct Entry {
	int line = 0;
	std::string name;
	std::vector<Chord> chords; // [shortcuts], [overview] and [commands]
	std::string value;         // [commands]: the command text
	bool none = false;         // `= none`: the action has no keys
};

struct Command {
	Chord chord;
	std::string command;

	friend bool operator==(const Command &, const Command &) = default;
};

// The effective settings: one key list per registry action (registry order),
// the commands in file order, and one member per row of `fields` below. The
// constructor puts every setting at its default.
struct Resolved {
	Resolved();

	std::vector<std::vector<Chord>> keys;
	std::vector<Command> commands;
	SwipePreset swipes{};
	bool pinch_scale{};
	// The direction switches, each true when set the other way from its
	// default (input/gestures.md).
	bool mouse_traditional{};
	bool touchpad_traditional{};
	bool pan_traditional{};
	bool overview_down{};
	bool hops_away{};
	int lock_idle_minutes{}; // 0: no idle lock
	bool night_light{};
	int night_light_k{};

	friend bool operator==(const Resolved &, const Resolved &) = default;
};

enum class Kind {
	choice,  // one of `names`
	toggle,  // `on` or `off`
	integer, // a whole number from `min` to `max`
};

// One setting of [gestures] or [session]. The table below is the one place
// a setting's key, its settings.get/settings.set name, its allowed values,
// its default and the Resolved member that holds it are written: parse,
// resolve, template_text, File::set, settings.get, settings.set and --help
// all read it.
//
// A setting's value is an int: a choice's is the index of its name, a
// toggle's is 0 (off) or 1 (on), an integer's is itself. The member is a
// SwipePreset, a bool (0 or 1) or an int.
struct Field {
	std::string_view section; // "gestures" or "session"
	std::string_view key;     // in the file
	std::string_view param;   // in settings.get and settings.set
	std::string_view help;    // what it does, in one line
	Kind kind;
	std::array<std::string_view, 3> names{}; // a choice's values, the first its way
	std::string_view unit{};  // an integer: what it counts
	int min = 0, max = 0;     // an integer: the lowest and highest number the file writes
	bool off = false;         // an integer: `off` is 0, and the only way the file writes it
	int fallback = 0;         // the value while the file does not say
	std::variant<bool Resolved::*, int Resolved::*, SwipePreset Resolved::*> member;

	// A choice's names; a toggle's, off then on.
	std::span<const std::string_view> values() const {
		static constexpr std::array<std::string_view, 2> on_off = {"off", "on"};
		if (kind == Kind::toggle) {
			return on_off;
		}
		std::size_t count = 0;
		while (count < names.size() && !names[count].empty()) {
			count++;
		}
		return {names.data(), count};
	}
	int read(const Resolved &resolved) const;
	void write(Resolved &resolved, int value) const;
};

// [gestures] first, then [session]; a section's rows are together, in the
// order the template and --help list them.
inline constexpr std::array<Field, 10> fields = {{
	{.section = "gestures", .key = "swipes", .param = "swipes",
		.help = "the fingers that hop between desktops, the other count opens the overview; "
			"off leaves 3 and 4 fingers to the clients",
		.kind = Kind::choice, .names = {"three-finger-hops", "four-finger-hops", "off"},
		.member = &Resolved::swipes},
	{.section = "gestures", .key = "pinch-scale", .param = "pinchScale",
		.help = "a Super + 2-finger pinch scales a desktop; off makes it a plain pinch",
		.kind = Kind::toggle, .fallback = 1, .member = &Resolved::pinch_scale},
	{.section = "gestures", .key = "mouse-scroll", .param = "mouseScroll",
		.help = "which way the mouse wheel scrolls apps",
		.kind = Kind::choice, .names = {"natural", "traditional"},
		.member = &Resolved::mouse_traditional},
	{.section = "gestures", .key = "touchpad-scroll", .param = "touchpadScroll",
		.help = "which way the touchpad scrolls apps",
		.kind = Kind::choice, .names = {"natural", "traditional"},
		.member = &Resolved::touchpad_traditional},
	{.section = "gestures", .key = "pan", .param = "pan",
		.help = "which way Super + scroll looks around",
		.kind = Kind::choice, .names = {"natural", "traditional"},
		.member = &Resolved::pan_traditional},
	{.section = "gestures", .key = "overview-swipe", .param = "overviewSwipe",
		.help = "the swipe that opens the overview",
		.kind = Kind::choice, .names = {"up", "down"}, .member = &Resolved::overview_down},
	{.section = "gestures", .key = "hop-swipe", .param = "hopSwipe",
		.help = "which way a swipe hops, toward the fingers or away from them",
		.kind = Kind::choice, .names = {"toward", "away"}, .member = &Resolved::hops_away},
	{.section = "session", .key = "lock-idle", .param = "lockIdleMinutes",
		.help = "minutes of idleness before the screen locks",
		.kind = Kind::integer, .unit = "minutes", .min = 1,
		// The idle timer takes milliseconds in an int.
		.max = std::numeric_limits<int>::max() / (60 * 1000), .off = true, .fallback = 5,
		.member = &Resolved::lock_idle_minutes},
	{.section = "session", .key = "night-light", .param = "nightLight",
		.help = "warm the screen",
		.kind = Kind::toggle, .member = &Resolved::night_light},
	{.section = "session", .key = "night-light-temperature", .param = "nightLightTemperature",
		.help = "the night light's colour temperature, warm at the low end",
		.kind = Kind::integer, .unit = "K", .min = 1000, .max = 6500, .fallback = 4000,
		.member = &Resolved::night_light_k},
}};

// The row `param` names, checked while compiling.
consteval const Field &field(std::string_view param) {
	for (const Field &each : fields) {
		if (each.param == param) {
			return each;
		}
	}
	std::unreachable(); // a param no row has: not a constant expression
}
// The rows other code names.
inline constexpr const Field &lock_idle = field("lockIdleMinutes");
inline constexpr const Field &night_light = field("nightLight");
inline constexpr const Field &night_light_temperature = field("nightLightTemperature");

// The row of `key` in `section`, or nullptr.
const Field *find_field(std::string_view section, std::string_view key);
// The row `param` names, or nullptr.
const Field *find_param(std::string_view param);
// The value the file's `text` spells for `field`, or nullopt when it is not
// one the field allows.
std::optional<int> parse_value(const Field &field, std::string_view text);
// The text the file writes `value` as.
std::string value_text(const Field &field, int value);
// Whether the settings may take `value`: a choice's index, a toggle's 0 or 1,
// an integer in its bounds, or 0 where the field has `off`.
bool allows(const Field &field, long long value);
// The values the field takes, in a phrase: `three-finger-hops, four-finger-hops
// or off`, `1000 to 6500 K`.
std::string allowed(const Field &field);
// Every field with its values, default and help, section by section, for
// the --help text.
std::string fields_help();

// A `key = value` line of [gestures] or [session] that parsed cleanly.
struct Setting {
	int line = 0;
	const Field *field = nullptr;
	int value = 0;
};

// A parsed canvas.conf: the clean entries of each section in file order, and
// every bad line with its number. Bad lines are ignored; the rest applies.
struct Parsed {
	std::vector<Entry> shortcuts;
	std::vector<Entry> overview;
	std::vector<Entry> commands;
	std::vector<Setting> settings; // [gestures] and [session]
	std::vector<Error> errors;
};

Parsed parse(std::string_view text);

// Resolution per scope: an action's keys are its line's when the file has
// one, else its defaults; a chord the file gives to an action or command is
// taken from any action that has it only by default; a repeated chord on a
// later line of one scope was dropped at parse time. A setting is its first
// line's, else its default.
Resolved resolve(const Parsed &parsed);

// The first-run file: a header comment, then every section with every action
// and every setting at its default, one line each, in registry order and
// the table's.
std::string template_text();

} // namespace settings
