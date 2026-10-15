#pragma once

#include <cstdint>
#include "util.h"

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
	// True when sym is an overview key and its action has run. Every key is
	// compositor-owned in the overview; callers swallow unmatched ones too.
	bool handle_overview(xkb_keysym_t sym, uint32_t mods, Output &output);

private:
	Server &server_;
};
