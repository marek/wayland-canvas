#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Text helpers without wlroots, linked by the compositor and by the agent usage collector.
namespace text {

constexpr char ascii_lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c;
}

constexpr bool ci_equal(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); i++) {
		if (ascii_lower(a[i]) != ascii_lower(b[i])) {
			return false;
		}
	}
	return true;
}

constexpr bool ci_starts_with(std::string_view s, std::string_view prefix) {
	return s.size() >= prefix.size() && ci_equal(s.substr(0, prefix.size()), prefix);
}

// An empty needle is contained in nothing.
constexpr bool ci_contains(std::string_view s, std::string_view needle) {
	if (needle.empty()) {
		return false;
	}
	for (std::size_t i = 0; i + needle.size() <= s.size(); i++) {
		if (ci_starts_with(s.substr(i), needle)) {
			return true;
		}
	}
	return false;
}

// `text` in single quotes for /bin/sh.
std::string shell_quote(std::string_view text);

// `text` as a JSON string literal, quotes included, for splicing into hand-built JSON:
// json-glib builders cannot hold a bare string as the root.
std::string json_quote(std::string_view text);

// The whole of `text` as a decimal integer; nullopt for anything else.
std::optional<int> parse_int(std::string_view text);

// The non-empty items of `value` split at `separator`, spaces and tabs trimmed.
std::vector<std::string> split_list(std::string_view value, char separator);

using IniEntries = std::vector<std::pair<std::string, std::string>>;

// The key=value lines under one [group] header, in file order.
struct IniGroup {
	std::string name;
	IniEntries entries;

	// The first value of `key`, exact case; null when the group has none.
	const std::string *get(std::string_view key) const;
};

// A key-file as the desktop entry and icon theme specifications read it: groups of
// key=value lines, `#` comments, LF or CRLF line ends. Lines before the first group header,
// lines without `=` and keys with no value are skipped; a NUL ends the file. Values are the
// raw text, spaces and tabs trimmed: escapes and lists are the caller's.
struct Ini {
	std::vector<IniGroup> groups;

	// The first group named `name`; null when the file has none.
	const IniGroup *group(std::string_view name) const;
};

Ini parse_ini(std::string_view contents);

} // namespace text
