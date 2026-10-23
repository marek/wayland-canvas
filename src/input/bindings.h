#pragma once

#include <cstdint>
#include <vector>

#include "core/settings.h"
#include "core/util.h"

class Output;
class Server;

// The keyboard shortcuts as the resolved settings (core/settings.h) hold
// them: the global table, the overview table, the [commands] chords and the
// lone Super tap. An action's `reach` decides where its chords apply; those
// of an `everywhere` action (the capture group) are tried before all else, so
// they work in the overview and while the capture UI holds the keyboard. VT
// switching is in no file and no chord can take it.
class Bindings {
public:
	explicit Bindings(Server &server);
	Bindings(const Bindings &) = delete;
	Bindings &operator=(const Bindings &) = delete;

	// True when sym with mods is a compositor shortcut, and its action has
	// run: the chords that reach everywhere first, then VT switching, the
	// global table and the [commands] chords.
	bool handle(xkb_keysym_t sym, uint32_t mods, Output *output);
	// The only chords a locked session answers: VT switching, so the way
	// out of a stuck session stays ([docs/intents/shell/lock.md]).
	bool handle_locked(xkb_keysym_t sym, uint32_t mods, Output *output);
	// True when sym with mods is a chord of an action that reaches
	// everywhere (shots, recordings, the toolbar), and its action has run.
	bool handle_everywhere(xkb_keysym_t sym, uint32_t mods, Output *output);
	// True when sym with mods is a key the overview owns, and its action
	// has run: the [overview] keys first, then the chords that reach
	// everywhere, those of the global actions that reach the overview (the
	// launcher, the terminal) and the [commands] chords. Unmatched keys are
	// the caller's to swallow: the overview owns the keyboard.
	bool handle_overview(xkb_keysym_t sym, uint32_t mods, Output &output);
	// The lone Super tap, or with `twice` the double tap: runs the action
	// whose keys hold `Super` (`Super Super`) — the overview's first when
	// that output is in the overview, then the global one. True when an
	// action ran.
	bool handle_tap(Output *output, bool twice);
	// True when an action holds the double tap, so a single tap waits to be
	// told from the first of two.
	bool has_double_tap(Output *output) const;
	// True when sym with mods is one of the launcher's own chords: while
	// the launcher is open they close it.
	bool launcher_chord(xkb_keysym_t sym, uint32_t mods) const;

	// Rebuilds every table from the resolved settings.
	void apply(const settings::Resolved &resolved);

	// True when something runs the registry action: every action has a
	// runner, so a key bound to it never reaches the client instead.
	static bool has_runner(const settings::ActionDef &def);
	// The table an action's chords are put in. It follows from the
	// action's scope and reach, never from its group label.
	enum class Table { everywhere, global, overview };
	static Table table_of(const settings::ActionDef &def);

private:
	// One resolved chord: the runner it runs (`runner`, an index into the
	// runner table of bindings.cpp), or a [commands] entry (`command`, an
	// index into command_lines_). `key` 0 is the Super tap, and with
	// `twice` the double tap. `in_overview` marks a global chord the
	// overview takes too.
	struct Bind {
		uint32_t mods = 0;
		xkb_keysym_t key = 0;
		bool twice = false;
		int runner = -1;
		int command = -1;
		bool in_overview = false;
	};

	// The bind for sym (any case) with mods exactly, or null.
	const Bind *find(xkb_keysym_t sym, uint32_t mods,
		const std::vector<Bind> &table) const;
	// The bind holding the tap (`twice`: the double tap) for that output.
	const Bind *tap_bind(Output *output, bool twice) const;
	// Runs a bind's runner or command.
	void run(const Bind &bind, Output *output);
	// Runs the bind of `table` for sym (any case) with mods exactly. True
	// when one ran.
	bool run_found(const std::vector<Bind> &table, xkb_keysym_t sym,
		uint32_t mods, Output *output);

	Server &server_;
	std::vector<Bind> global_;     // Table::global: [shortcuts] actions, registry order
	std::vector<Bind> everywhere_; // Table::everywhere: tried before all else
	std::vector<Bind> overview_;   // Table::overview: [overview] actions
	std::vector<Bind> commands_;   // the [commands] chords
	std::vector<std::string> command_lines_;
};
