#pragma once

#include <cstdint>
#include <expected>
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

// The table a chord is written for: [shortcuts] or [overview].
enum class Scope { global, overview };

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

// `text` without the spaces, tabs and carriage returns at both ends.
std::string_view trim(std::string_view text);

// `text` cut at every `sep`, each part trimmed; an empty part stays.
std::vector<std::string_view> split(std::string_view text, char sep);

// Canonical chord text: modifiers in Super, Ctrl, Alt, Shift order, then the
// key (`Super+Shift+K`); `Super` alone is the tap.
std::string chord_text(Chord chord);

// The chords as the file writes them: `Super+Q, Alt+F4`.
std::string chords_text(const std::vector<Chord> &chords);

// Parses one chord, case-insensitively. `Super` alone is the tap; another
// lone modifier is an error. Global scope also demands Super, Ctrl or Alt,
// or a key that types no character; the error says why, in one line.
std::expected<Chord, std::string> parse_chord(std::string_view text, Scope scope);

} // namespace settings
