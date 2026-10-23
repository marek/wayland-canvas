#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>
#include "core/chord.h"
#include "core/util.h"

class Server;
class SuperTap;

// A shortcut capture, for the settings window: from begin to end every key,
// modifiers included, belongs to it — nothing runs and no surface sees a
// key. The first non-modifier key ends it as the chord held, a lone Super
// release is the tap (twice, the double tap), and a bare Escape cancels
// (docs/intents/shell/settings.md).
class ShortcutCapture {
public:
	// Receives the chord pressed, or nullopt when the capture was cancelled
	// (Escape alone, or a second capture).
	using Done = std::function<void(std::optional<settings::Chord>)>;

	ShortcutCapture(Server &server, SuperTap &tap);
	ShortcutCapture(const ShortcutCapture &) = delete;
	ShortcutCapture &operator=(const ShortcutCapture &) = delete;

	void finish(); // before the event loop goes

	// One capture at a time: a second cancels the first.
	void begin(Done done);
	// Ends the capture under way, if any; it answers nullopt.
	void cancel();
	// Whether the capture takes this key event: a capture under way takes
	// every key, and one just answered takes the release of each key held
	// at the end, so nothing of the chord runs after it.
	bool takes(const wlr_keyboard_key_event &event, std::span<const xkb_keysym_t> syms);

private:
	// A capture under way until the chord is pressed or it is cancelled.
	struct Capture {
		Done done;
		uint32_t mods = 0; // modifiers held inside the capture
		bool tapped = false; // a lone Super tap: one, or the first of two
		bool ending = false; // answered; the keys held at the end drain
		std::vector<uint32_t> draining; // their keycodes
	};

	// Every key of a capture under way: modifiers are tracked, the first
	// non-modifier key ends it as the chord, a lone Super release is the
	// tap, and a bare Escape cancels.
	void key(const wlr_keyboard_key_event &event, std::span<const xkb_keysym_t> syms);
	// Answers the capture — the chord pressed, or nothing on a cancel —
	// after which the keys held at the end drain before it is gone.
	void end(std::optional<settings::Chord> chord);
	void arm_timer();
	static int handle_timer(void *data);

	Server &server_;
	SuperTap &tap_;
	std::optional<Capture> capture_;
	EventSourcePtr timer_;
};
