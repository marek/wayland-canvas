#pragma once

#include <deque>
#include <functional>
#include "core/util.h"

class Server;

// Input that waits for X (docs/intents/windows/x11.md): input for an X11
// window waits until X has answered, in order, within a time limit. Touch
// and the pen queue behind it as the pointer does.
class XInputGate {
public:
	explicit XInputGate(Server &server);
	XInputGate(const XInputGate &) = delete;
	XInputGate &operator=(const XInputGate &) = delete;

	void finish(); // shutting down: nothing waiting runs

	// Whether `event` waits, to run through `Handler` later and in order:
	// one waits already, or it is for an X11 window (`for_x`: a press, a
	// scroll, a touch or a pen tip there) while X still has window changes
	// to take in.
	template <auto Handler, typename Owner, typename Event>
	bool waits(Owner *owner, const Event *event, bool for_x);
	// The same for an event without data: a frame.
	template <auto Handler, typename Owner>
	bool waits(Owner *owner);
	// Whatever waits runs now, in order: before a device goes, since a
	// waiting event names it.
	void run_waiting();

private:
	bool must_wait(bool for_x);
	void queue(std::function<void()> run);
	// Runs what waits, in order, until a press for X finds X still behind
	// (it goes first again) or, `forced`, everything.
	void release_waiting(bool forced);
	static int handle_timer(void *data);

	Server &server_;
	std::deque<std::function<void()>> waiting_;
	bool releasing_ = false, requeued_ = false, forced_ = false;
	EventSourcePtr timer_;
	double since_ = 0;
};

template <auto Handler, typename Owner, typename Event>
bool XInputGate::waits(Owner *owner, const Event *event, bool for_x) {
	if (!must_wait(for_x)) {
		return false;
	}
	queue([owner, copy = *event]() mutable { (owner->*Handler)(&copy); });
	return true;
}

template <auto Handler, typename Owner>
bool XInputGate::waits(Owner *owner) {
	if (!must_wait(false)) {
		return false;
	}
	queue([owner] { (owner->*Handler)(); });
	return true;
}
