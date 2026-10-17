#pragma once

#include <cstdint>
#include <vector>

#include "core/settings.h"
#include "core/util.h"

class Output;
class Server;

// The keyboard shortcuts, as the resolved settings (core/settings.h) hold
// them: the global table everywhere but in the overview, the overview
// table in it, the [commands] chords, and the lone Super tap. The capture
// group's chords are tried before everything, so they work in the overview
// and while the capture UI holds the keyboard. VT switching is in no file
// and no chord can take it.
class Bindings {
public:
	explicit Bindings(Server &server);
	Bindings(const Bindings &) = delete;
	Bindings &operator=(const Bindings &) = delete;

	// True when sym with mods is a compositor shortcut, and its action has
	// run: the capture chords first, then VT switching, the global table
	// and the [commands] chords.
	bool handle(xkb_keysym_t sym, uint32_t mods, Output *output);
	// True when sym with mods is a capture chord (shots, recordings, the
	// toolbar), and its action has run.
	bool handle_capture(xkb_keysym_t sym, uint32_t mods, Output *output);
	// True when sym with mods is a key the overview owns, and its action
	// has run: the [overview] keys first, then the capture chords, the
	// launcher, the terminal and the [commands] chords. Unmatched keys are
	// the caller's to swallow: the overview owns the keyboard.
	bool handle_overview(xkb_keysym_t sym, uint32_t mods, Output &output);
	// The lone Super tap: runs the action whose keys hold `Super` — the
	// overview's first when that output is in the overview, then the
	// global one. True when an action ran.
	bool handle_tap(Output *output);
	// True when sym with mods is one of the launcher's own chords: while
	// the launcher is open they close it.
	bool launcher_chord(xkb_keysym_t sym, uint32_t mods) const;

	// Rebuilds every table from the resolved settings.
	void apply(const settings::Resolved &resolved);

private:
	// One resolved chord: the registry action it runs (`action`, an index
	// into settings::registry()), or a [commands] entry (`command`, an
	// index into command_lines_). `key` 0 is the Super tap.
	struct Bind {
		uint32_t mods = 0;
		xkb_keysym_t key = 0;
		int action = -1;
		int command = -1;
	};

	// The bind for sym (any case) with mods exactly, or null.
	const Bind *find(xkb_keysym_t sym, uint32_t mods,
		const std::vector<Bind> &table) const;
	// Runs a bind's action or command. True when one ran.
	bool run(const Bind &bind, Output *output);

	Server &server_;
	std::vector<Bind> global_;   // registry [shortcuts] actions, registry order
	std::vector<Bind> capture_;  // the Capture group, tried before all else
	std::vector<Bind> overview_; // registry [overview] actions
	std::vector<Bind> commands_; // the [commands] chords
	std::vector<std::string> command_lines_;
	int launcher_ = -1; // registry indices of the launcher and the terminal,
	int terminal_ = -1; // held for the keys that work in the overview
};
