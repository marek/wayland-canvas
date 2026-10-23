#include "input/super_tap.h"

#include "core/config.h"
#include "core/server.h"

SuperTap::SuperTap(Server &server) : server_(server) {}

void SuperTap::finish() {
	timer_.reset();
}

void SuperTap::key_pressed(bool logo, bool alone) {
	if (!logo) {
		run_pending(); // a key ends the interval: the tap was one
	}
	forming_ = logo && alone;
}

void SuperTap::disarm() {
	forming_ = false;
}

bool SuperTap::key_released(bool logo) {
	bool tap = forming_ && logo;
	forming_ = false;
	return tap;
}

void SuperTap::other_input() {
	forming_ = false;
	run_pending();
}

void SuperTap::lone_tap() {
	Output *output = server_.output_under_cursor();
	if (!server_.bindings.has_double_tap(output)) {
		server_.bindings.handle_tap(output, false);
		return;
	}
	if (pending_) {
		cancel_pending();
		server_.bindings.handle_tap(output, true);
		return;
	}
	pending_ = true;
	arm_timer();
}

void SuperTap::run_pending() {
	if (!pending_) {
		return;
	}
	cancel_pending();
	if (server_.lock.locked()) {
		return;
	}
	server_.bindings.handle_tap(server_.output_under_cursor(), false);
}

void SuperTap::cancel_pending() {
	pending_ = false;
	if (timer_ != nullptr) {
		wl_event_source_timer_update(timer_.get(), 0);
	}
}

void SuperTap::arm_timer() {
	if (timer_ == nullptr) {
		timer_.reset(wl_event_loop_add_timer(server_.loop, &SuperTap::handle_timer, this));
	}
	wl_event_source_timer_update(timer_.get(), static_cast<int>(config::double_tap_ms));
}

// The interval passed without a second tap: the one tap was all.
int SuperTap::handle_timer(void *data) {
	static_cast<SuperTap *>(data)->run_pending();
	return 0;
}
