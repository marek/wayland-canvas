#include "core/text.h"

#include <charconv>
#include <format>
#include <iterator>

namespace text {

namespace {

std::string_view trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
		s.remove_suffix(1);
	}
	return s;
}

} // namespace

std::string shell_quote(std::string_view text) {
	std::string out = "'";
	for (const char c : text) {
		if (c == '\'') {
			out += "'\\''";
		} else {
			out += c;
		}
	}
	out += '\'';
	return out;
}

std::string json_quote(std::string_view text) {
	std::string out;
	out.reserve(text.size() + 2);
	out += '"';
	for (const char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				std::format_to(std::back_inserter(out), "\\u{:04x}",
					static_cast<unsigned char>(c));
			} else {
				out += c;
			}
		}
	}
	out += '"';
	return out;
}

std::optional<int> parse_int(std::string_view text) {
	int value = 0;
	const char *end = text.data() + text.size();
	const auto [stop, error] = std::from_chars(text.data(), end, value);
	if (error != std::errc{} || stop != end) {
		return std::nullopt;
	}
	return value;
}

std::vector<std::string> split_list(std::string_view value, char separator) {
	std::vector<std::string> items;
	while (true) {
		const std::size_t at = value.find(separator);
		const std::string_view item = trim(value.substr(0, at));
		if (!item.empty()) {
			items.emplace_back(item);
		}
		if (at == std::string_view::npos) {
			return items;
		}
		value.remove_prefix(at + 1);
	}
}

const std::string *IniGroup::get(std::string_view key) const {
	for (const auto &[entry_key, value] : entries) {
		if (entry_key == key) {
			return &value;
		}
	}
	return nullptr;
}

const IniGroup *Ini::group(std::string_view name) const {
	for (const IniGroup &each : groups) {
		if (each.name == name) {
			return &each;
		}
	}
	return nullptr;
}

Ini parse_ini(std::string_view contents) {
	if (const std::size_t nul = contents.find('\0'); nul != std::string_view::npos) {
		contents = contents.substr(0, nul);
	}
	Ini ini;
	std::size_t pos = 0;
	while (pos < contents.size()) {
		const std::size_t eol = contents.find('\n', pos);
		std::string_view line = eol == std::string_view::npos
			? contents.substr(pos) : contents.substr(pos, eol - pos);
		pos = eol == std::string_view::npos ? contents.size() : eol + 1;
		if (line.ends_with('\r')) {
			line.remove_suffix(1);
		}
		line = trim(line);
		if (line.empty() || line.front() == '#') {
			continue;
		}
		if (line.front() == '[') {
			// a header that does not close opens a group no lookup names
			ini.groups.push_back({line.back() == ']' && line.size() > 1
				? std::string(line.substr(1, line.size() - 2)) : std::string{}, {}});
			continue;
		}
		const std::size_t eq = line.find('=');
		if (ini.groups.empty() || eq == std::string_view::npos) {
			continue;
		}
		const std::string_view key = trim(line.substr(0, eq));
		const std::string_view value = trim(line.substr(eq + 1));
		if (!key.empty() && !value.empty()) {
			ini.groups.back().entries.emplace_back(key, value);
		}
	}
	return ini;
}

} // namespace text
