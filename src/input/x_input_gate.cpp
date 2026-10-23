#include "input/x_input_gate.h"

#include <utility>
#include "core/config.h"
#include "core/server.h"

XInputGate::XInputGate(Server &server) : server_(server) {}

void XInputGate::finish() {
	waiting_.clear();
	timer_.reset();
}

bool XInputGate::must_wait(bool for_x) {
	if (forced_) {
		return false;
	}
	// Replaying, the events are already in order: only a press for X that
	// X is behind on again stops them.
	if (!releasing_ && !waiting_.empty()) {
		return true;
	}
	return for_x && !server_.x11_settled();
}

void XInputGate::queue(std::function<void()> run) {
	if (releasing_) {
		waiting_.push_front(std::move(run)); // it goes first again
		requeued_ = true;
		return;
	}
	if (waiting_.empty()) {
		if (!timer_) {
			timer_.reset(wl_event_loop_add_timer(server_.loop, &XInputGate::handle_timer,
				this));
		}
		since_ = now_ms();
		wl_event_source_timer_update(timer_.get(), 1);
	}
	waiting_.push_back(std::move(run));
}

// Every millisecond while input waits: X's answer read, or the wait over.
int XInputGate::handle_timer(void *data) {
	XInputGate &gate = *static_cast<XInputGate *>(data);
	bool late = now_ms() - gate.since_ >= config::x11_input_wait_ms;
	if (!late && !gate.server_.x11_settled()) {
		wl_event_source_timer_update(gate.timer_.get(), 1);
		return 0;
	}
	if (late) {
		gate.server_.x11_give_up();
	}
	gate.release_waiting(late);
	return 0;
}

void XInputGate::release_waiting(bool forced) {
	if (timer_) {
		wl_event_source_timer_update(timer_.get(), 0);
	}
	releasing_ = true;
	forced_ = forced;
	requeued_ = false;
	while (!waiting_.empty() && !requeued_) {
		std::function<void()> next = std::move(waiting_.front());
		waiting_.pop_front();
		next();
	}
	releasing_ = false;
	forced_ = false;
	if (requeued_) {
		// A replayed move sent X new changes: the press waits for those.
		requeued_ = false;
		since_ = now_ms();
		wl_event_source_timer_update(timer_.get(), 1);
	}
}

void XInputGate::run_waiting() {
	if (!waiting_.empty()) {
		release_waiting(true);
	}
}
