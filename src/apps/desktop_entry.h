#pragma once

#include <optional>
#include <string>
#include <string_view>

// Pure XDG desktop-entry handling (no wlroots), unit-tested in tests/test_desktop_entry.cpp.

struct App {
	std::string id; // desktop file id, e.g. "org.gnome.Nautilus.desktop"
	std::string name; // Name, localized for the parse's lang when the file has it
	std::string generic; // GenericName; empty when absent
	std::string keywords; // Keywords (';'-separated as in the file); empty when absent
	std::string exec; // Exec with field codes removed, for /bin/sh -c
	std::string icon; // Icon: theme name or absolute path; empty when absent
	std::string try_exec; // TryExec; empty when absent. Entries whose binary is missing are dropped
	bool terminal = false; // Terminal=true

	// Parses the [Desktop Entry] group of a .desktop file. `lang` is the locale (e.g.
	// "de_DE.UTF-8", "de"; empty or "C" for none): Name[de_DE]/Name[de] win over Name.
	// nullopt for entries that must not be listed: Type other than Application,
	// NoDisplay=true, Hidden=true, or a missing Name or Exec.
	static std::optional<App> parse(std::string_view contents, std::string_view id,
		std::string_view lang);

	// How well `query` (case-insensitive, may be empty) matches: -1 no match, otherwise
	// higher is better; an empty query matches everything equally. Ranking: name prefix >
	// word prefix in name > substring of name > generic name / keywords / exec match.
	int match(std::string_view query) const;
};

// Exec value with %f %F %u %U %d %D %n %N %i %c %k %v %m removed and "%%" turned into "%".
std::string clean_exec(std::string_view exec);
