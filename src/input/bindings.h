#pragma once

#include <cstdint>
#include "core/util.h"

class Output;
class Server;

// The keyboard shortcuts: what each key does, globally and in the overview.
class Bindings {
public:
	explicit Bindings(Server &server);
	Bindings(const Bindings &) = delete;
	Bindings &operator=(const Bindings &) = delete;

	// True when sym with mods is a compositor shortcut; its action has run.
	bool handle(xkb_keysym_t sym, uint32_t mods, Output *output);
	// True when sym with mods is a capture chord (shots, recordings, the
	// toolbar); its action has run. Tried before the overview keys, so the
	// capture chords work there too, and by the capture UI itself, so the
	// chord that opened something closes it again.
	bool handle_capture(xkb_keysym_t sym, uint32_t mods, Output *output);
	// True when sym is an overview key and its action has run. Every key is
	// compositor-owned in the overview; callers swallow unmatched ones too.
	bool handle_overview(xkb_keysym_t sym, uint32_t mods, Output &output);

private:
	Server &server_;
};
