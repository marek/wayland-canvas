// chord.cpp: a key chord and canvas.conf's text rules for it: trim and split,
// the chord's text and its parse. Pure: no wlroots, no Server; xkbcommon
// names the keys.

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/chord.h"

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

} // namespace settings
