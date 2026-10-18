#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace settings {

// Modifier bits, the same values wlroots' WLR_MODIFIER_* use, so a chord
// matches the mask wlr_keyboard_get_modifiers() produces.
inline constexpr uint32_t mod_shift = 1u << 0;
inline constexpr uint32_t mod_ctrl = 1u << 2;
inline constexpr uint32_t mod_alt = 1u << 3;
inline constexpr uint32_t mod_logo = 1u << 6;

enum class Scope { global, overview };

// What the [gestures] section chose. GestureOptions (input/gesture.h) carries
// the same choice into the gesture code; this library stays wlroots-free, so
// server.cpp maps between the two.
enum class SwipePreset { three_finger_hops, four_finger_hops, off };

// [session] bounds and defaults. The idle lock is in minutes, 0 for off;
// the night light's temperature in kelvin, warm at the low end.
inline constexpr int default_lock_idle_minutes = 5;
inline constexpr int night_light_min_k = 1000;
inline constexpr int night_light_max_k = 6500;
inline constexpr int default_night_light_k = 4000;

// Modifiers then one key. `key` 0 with `mods` the logo bit is the lone-Super
// tap, and with `twice` the double tap; parse_chord makes no other
// modifier-only chord.
struct Chord {
	uint32_t mods = 0;
	xkb_keysym_t key = 0;
	bool twice = false;

	bool tap() const { return key == 0; } // the lone tap or the double tap
	friend bool operator==(const Chord &, const Chord &) = default;
};

// One action of the registry (docs/plans' settings contract §3), in registry
// order with its group and label.
struct ActionDef {
	Scope scope;
	std::string_view name;
	std::string_view label;
	std::string_view group;
	std::vector<Chord> defaults;
};

// Every action in registry order: the [shortcuts] ones first, the [overview]
// ones last with group "In the overview".
const std::vector<ActionDef> &registry();

// One bad line of canvas.conf, with its 1-based line number.
struct Error {
	int line = 0;
	std::string text;
};

// A `name = value` line that parsed cleanly, with its 1-based line number.
// `name` is the action name, the gesture setting's name, or the chord text
// ([commands], where the chord names the line).
struct Entry {
	int line = 0;
	std::string name;
	std::vector<Chord> chords; // [shortcuts] and [overview]
	std::string value;         // [commands] command text, [gestures] value
	bool none = false;         // `= none`: the action has no keys
};

struct Command {
	Chord chord;
	std::string command;

	friend bool operator==(const Command &, const Command &) = default;
};

// A parsed canvas.conf: the clean entries of each section in file order, and
// every bad line with its number. Bad lines are ignored; the rest applies.
struct Parsed {
	std::vector<Entry> shortcuts;
	std::vector<Entry> overview;
	std::vector<Entry> commands;
	std::vector<Entry> gestures;
	std::vector<Entry> session; // [session]: the idle lock, the night light
	std::vector<Error> errors;
};

Parsed parse(std::string_view text);

// The effective settings: one key list per registry action (registry order),
// the commands in file order, and the gesture choices.
struct Resolved {
	std::vector<std::vector<Chord>> keys;
	std::vector<Command> commands;
	SwipePreset swipes = SwipePreset::three_finger_hops;
	bool pinch_scale = true;
	// [gestures] invert: on reverses the pointer and touchpad scroll
	// direction and the direction a swipe hop takes (input/gestures.md).
	bool invert = false;
	int lock_idle_minutes = default_lock_idle_minutes; // 0: no idle lock
	bool night_light = false;
	int night_light_k = default_night_light_k;

	friend bool operator==(const Resolved &, const Resolved &) = default;
};

// Resolution per scope: an action's keys are its line's when the file has
// one, else its defaults; a chord the file gives to an action or command is
// taken from any action that has it only by default; a repeated chord on a
// later line of one scope was dropped at parse time.
Resolved resolve(const Parsed &parsed);

// Canonical chord text: modifiers in Super, Ctrl, Alt, Shift order, then the
// key (`Super+Shift+K`); `Super` alone is the tap.
std::string chord_text(Chord chord);

// Parses one chord, case-insensitively. `Super` alone is the tap; another
// lone modifier is an error. Global scope also demands Super, Ctrl or Alt,
// or a key that types no character; the error says why, in one line.
std::expected<Chord, std::string> parse_chord(std::string_view text, Scope scope);

// The first-run file: a header comment, then every section with every action
// at its defaults, one line each, in registry order.
std::string template_text();

// What a settings.set took from another action or command: the loser and the
// chords that moved. `action` is empty when the loser is a [commands] entry,
// whose text then rides in `command`.
struct Moved {
	Scope scope = Scope::global;
	std::string action;
	std::vector<Chord> keys;
	std::string command;
};

// A settings.set's whole result: the new file text and what else it rewrote.
struct Edit {
	std::string text;
	std::vector<Moved> moved;
};

// canvas.conf as an editable document: the parsed file, its resolution, and
// the in-place edits `settings.set` makes. Every line the edit does not
// touch, comments included, stays byte for byte.
class File {
public:
	explicit File(std::string_view text);

	const Parsed &parsed() const { return parsed_; }
	const Resolved &resolved() const { return resolved_; }
	const std::string &text() const { return text_; }

	// Sets one action's keys: a value replaces its line or is appended at
	// the end of its section (an empty list writes `none`); `keys` nullopt
	// resets it, its defaults written into its line. Chords another action
	// or command of the scope also holds move: their lines are rewritten too.
	std::expected<Edit, std::string> set_action(Scope scope, std::string_view action,
		const std::optional<std::vector<Chord>> &keys);

	// Replaces the [commands] entries, keeping the section's comments.
	// A chord twice in `commands` is an error.
	std::expected<Edit, std::string> set_commands(const std::vector<Command> &commands);

	// One [session] setting, by its name in the file.
	std::expected<Edit, std::string> set_session(std::string_view name,
		std::string_view value);

	std::expected<Edit, std::string> set_swipes(SwipePreset preset);
	std::expected<Edit, std::string> set_pinch_scale(bool on);
	std::expected<Edit, std::string> set_invert(bool on);

private:
	// One on/off line of [gestures], by its name in the file.
	std::expected<Edit, std::string> set_gesture_switch(std::string_view name,
		bool on);

	Parsed parsed_;
	Resolved resolved_;
	std::string text_;
	std::vector<std::string> lines_; // text_ line by line, '\n' dropped
	bool trailing_newline_ = true;

	// The line indices of `section`'s entry lines (`name` when given, else
	// every entry), in file order.
	std::vector<int> entry_lines(std::string_view section,
		std::string_view name = {}) const;
	// The line a new entry of `section` goes after: its last non-blank line
	// before the next section header, the header itself when the section is
	// empty, one past the last line when the section is missing.
	int append_after(std::string_view section) const;
	// Rewrites line `index` to `name = value` (deleted when `value` is empty
	// and `del_empty`); every other line stays byte for byte.
	void write_line(int index, std::string_view name, std::string_view value,
		bool del_empty);
	// Appends `name = value` at the end of `section`, the section itself
	// appended at the end of the file when missing.
	void append_entry(std::string_view section, std::string_view name,
		std::string_view value);
	// The `section` header line, or -1 when the file has none.
	int section_header(std::string_view section) const;
	// Rebuilds text_ from lines_.
	void rejoin();

	// Rewrites the lines of the actions `chords` are taken from (a loser
	// holding them only by default has no line to rewrite) and reports them.
	void take_from_actions(const std::vector<Chord> &chords, Scope scope,
		std::string_view skip_action, std::vector<Moved> &moved);
	// Removes the lines of the commands `chords` are taken from, reported
	// with their command text.
	void take_from_commands(const std::vector<Chord> &chords,
		std::vector<Moved> &moved);
};

} // namespace settings
