// settings_file.cpp: canvas.conf as an editable document: the in-place edits
// settings.set makes. Pure: no wlroots, no Server.

#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/chord.h"
#include "core/settings.h"
#include "core/settings_file.h"

namespace settings {

namespace {

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
