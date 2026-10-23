#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/chord.h"
#include "core/settings.h"

namespace settings {

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

	// Sets one [gestures] or [session] setting: its line is rewritten or
	// appended at the end of its section. A value the field does not allow
	// is an error.
	std::expected<Edit, std::string> set(const Field &field, long long value);

private:
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
	// Writes `name = value` into `section` (an empty value writes `none`):
	// the first line that sets `name` is rewritten, any later duplicate is
	// dropped, and a name the file does not have is appended.
	void write_entry(std::string_view section, std::string_view name,
		std::string_view value);
	// Removes the lines at `lines`, ascending indices.
	void erase_lines(const std::vector<int> &lines);
	// Appends the `section` header at the end of the file when it has none.
	void ensure_section(std::string_view section);
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
