#pragma once

#include "core/util.h"

class Server;

// The Super tap (docs/intents/input/keyboard-and-pointer.md): the logo key
// pressed and released with nothing in between runs the action that holds
// `Super`, or `Super` twice within the double-tap interval runs the one
// that holds the double tap. Any other key, button, scroll or gesture ends
// a tap forming.
class SuperTap {
public:
	explicit SuperTap(Server &server);
	SuperTap(const SuperTap &) = delete;
	SuperTap &operator=(const SuperTap &) = delete;

	void finish(); // before the event loop goes

	// A key was pressed; `alone` when it is the only key held. A logo key
	// pressed alone may become a tap, any other key ends the interval a
	// tap waits out.
	void key_pressed(bool logo, bool alone);
	// The key being held is no tap: the session is locked, or the layer
	// that has the keyboard takes none.
	void disarm();
	// A key was released: whether that completes a tap, which `lone_tap`
	// then runs.
	bool key_released(bool logo);
	// A lone tap runs at once unless an action holds the double tap; then
	// the first waits out the interval, and a second within it is the
	// double tap.
	void lone_tap();
	// Any other input ends a tap: one forming is no tap, and one waiting to
	// be told from a double tap runs now.
	void other_input();
	// Drops a tap waiting to be told from a double tap, unrun.
	void cancel_pending();

private:
	void run_pending();
	void arm_timer();
	static int handle_timer(void *data);

	Server &server_;
	// Logo key pressed alone and nothing else happened yet: releasing it is
	// the tap.
	bool forming_ = false;
	// A lone tap waiting out the double-tap interval, since an action holds
	// the double tap; the timer runs it when no second tap comes.
	bool pending_ = false;
	EventSourcePtr timer_;
};
