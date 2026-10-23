// touch.cpp: the touchscreens (docs/intents/input/touch-and-pen.md). Each
// finger is decided as it lands: a client's when it lands on a client's
// surface, the pointer's when it is the only one on the compositor's UI, a
// gesture's when it makes two on the canvas or three or four anywhere.
#include "input/touch.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include "core/config.h"
#include "core/server.h"
#include "core/process.h"
#include "input/gesture.h"
#include "input/input.h"
#include "platform/output.h"

Touch::Device::Device(Touch &touch, wlr_input_device *device)
	: wlr(device), touch_(touch) {
	destroy_.connect<&Device::handle_destroy>(device->events.destroy, this);
}

void Touch::Device::handle_destroy() {
	touch_.device_gone(this);
}

Touch::Touch(Server &server, Input &input) : server_(server), input_(input) {}

Touch::~Touch() = default;

void Touch::init() {
	wlr_cursor *cursor = server_.cursor;
	down_.connect<&Touch::handle_down>(cursor->events.touch_down, this);
	up_.connect<&Touch::handle_up>(cursor->events.touch_up, this);
	motion_.connect<&Touch::handle_motion>(cursor->events.touch_motion, this);
	cancel_.connect<&Touch::handle_cancel>(cursor->events.touch_cancel, this);
	frame_.connect<&Touch::handle_frame>(cursor->events.touch_frame, this);
}

void Touch::finish() {
	down_.disconnect();
	up_.disconnect();
	motion_.disconnect();
	cancel_.disconnect();
	frame_.disconnect();
	hold_timer_.reset();
	contacts_.clear();
	devices_.clear();
}

void Touch::new_device(wlr_input_device *device) {
	wlr_cursor_attach_input_device(server_.cursor, device);
	devices_.push_back(std::make_unique<Device>(*this, device));
	map_devices();
	input_.update_capabilities();
}

void Touch::map_devices() {
	for (const std::unique_ptr<Device> &device : devices_) {
		wlr_touch *touch = wlr_touch_from_input_device(device->wlr);
		Output *output = input_.output_for_device(touch->output_name);
		wlr_cursor_map_input_to_output(server_.cursor, device->wlr,
			output != nullptr ? output->wlr : nullptr);
	}
}

void Touch::device_gone(Device *device) {
	input_.x_gate.run_waiting(); // a waiting event names this device
	end_all(static_cast<uint32_t>(now_ms()));
	destroy_owned(devices_, device);
	input_.update_capabilities();
}

Touch::Contact *Touch::find(int32_t id) {
	auto it = std::ranges::find(contacts_, id, &Contact::id);
	return it != contacts_.end() ? &*it : nullptr;
}

void Touch::place(wlr_touch *touch, double x, double y, double &lx, double &ly) const {
	wlr_cursor_absolute_to_layout_coords(server_.cursor, &touch->base, x, y, &lx, &ly);
}

bool Touch::centroid(double &cx, double &cy) const {
	double sx = 0, sy = 0;
	int n = 0;
	for (const Contact &contact : contacts_) {
		if (contact.role == Role::view) {
			sx += contact.lx;
			sy += contact.ly;
			n++;
		}
	}
	if (n == 0) {
		return false;
	}
	cx = sx / n;
	cy = sy / n;
	return true;
}

// ---- the events ----

void Touch::handle_down(wlr_touch_down_event *event) {
	// A finger on an X11 window waits until X has taken in its window moves.
	double lx = 0, ly = 0;
	place(event->touch, event->x, event->y, lx, ly);
	std::optional<PointerRouter::ContactHit> landed = input_.router.client_at(lx, ly);
	if (input_.x_gate.waits<&Touch::handle_down>(this, event,
			landed.has_value() && Xwm::is_x11_window(landed->surface))) {
		return;
	}
	input_.contact_activity();
	uint32_t time = event->time_msec;
	apply_moves(); // this frame's moves count before a new finger changes the gesture
	Contact contact{.id = event->touch_id};
	place(event->touch, event->x, event->y, contact.lx0, contact.ly0);
	contact.lx = contact.lx0;
	contact.ly = contact.ly0;
	std::size_t fingers = contacts_.size() + 1;
	// Locked, the locker takes every finger: no gesture, no compositor UI.
	if (!server_.lock.locked() && join_gesture(contact, fingers, time)) {
		return;
	}

	std::optional<PointerRouter::ContactHit> hit =
		input_.router.client_at(contact.lx, contact.ly);
	if (hit.has_value() && wlr_surface_accepts_touch(hit->surface, server_.seat)) {
		contact.role = Role::client;
		contact.sx = hit->sx;
		contact.sy = hit->sy;
		contact.scale = hit->scale;
		if (fingers == 1) {
			input_.router.contact_focus(*hit); // a first finger is a click: focus and raise
		}
		wlr_seat_touch_notify_down(server_.seat, hit->surface, time, contact.id,
			hit->sx, hit->sy);
		contacts_.push_back(contact);
		return;
	}

	if (fingers == 1) {
		// The compositor's UI, or a client with no touch support: the pointer
		// comes to the finger, and the press waits to be told a tap, a drag
		// or a hold.
		contact.role = Role::pointer;
		state_ = Press{};
		contacts_.push_back(contact);
		input_.router.contact_motion(time, contact.lx, contact.ly);
		arm_hold();
		return;
	}
	contacts_.push_back(contact); // nothing takes it: it is ignored until it lifts
}

bool Touch::join_gesture(Contact &contact, std::size_t fingers, uint32_t time) {
	if (const Swipe *swipe = std::get_if<Swipe>(&state_)) {
		contact.role = Role::view;
		contacts_.push_back(contact);
		// A fourth finger before the three have travelled: the four-finger swipe.
		if (fingers == 4 && swipe->fingers == 3 && swipe->travel <= config::drag_threshold) {
			server_.gestures.swipe_end(time, true);
			begin_swipe(time, 4);
		}
		return true;
	}
	// A layer owns the pointer, as it does for a touchpad swipe
	// (Input::handle_swipe_begin): the fingers beyond the first are ignored.
	if ((fingers == 3 || fingers == 4)
			&& !input_.gestures_blocked()
			&& server_.gestures.options().swipes != settings::SwipePreset::off) {
		contact.role = Role::view;
		contacts_.push_back(contact);
		begin_swipe(time, static_cast<uint32_t>(fingers));
		return true;
	}
	// A second finger on the canvas beside a first one waiting there: the
	// two move the view, and the first one's tap is off.
	const Press *press = std::get_if<Press>(&state_);
	if (fingers != 2 || press == nullptr || press->phase != Press::Phase::waiting
			|| !input_.router.canvas_at(contacts_[0].lx0, contacts_[0].ly0)
			|| !input_.router.canvas_at(contact.lx, contact.ly)) {
		return false;
	}
	if (hold_timer_) {
		wl_event_source_timer_update(hold_timer_.get(), 0);
	}
	contacts_[0].role = Role::view;
	contact.role = Role::view;
	contacts_.push_back(contact);
	TwoFinger two;
	centroid(two.cx0, two.cy0);
	two.cx = two.cx0;
	two.cy = two.cy0;
	two.dist0 = std::hypot(contacts_[0].lx - contacts_[1].lx,
		contacts_[0].ly - contacts_[1].ly);
	state_ = two;
	return true;
}

void Touch::handle_motion(wlr_touch_motion_event *event) {
	if (input_.x_gate.waits<&Touch::handle_motion>(this, event, false)) {
		return;
	}
	Contact *contact = find(event->touch_id);
	if (contact == nullptr) {
		return;
	}
	input_.contact_activity();
	uint32_t time = event->time_msec;
	place(event->touch, event->x, event->y, contact->lx, contact->ly);
	double lx = contact->lx, ly = contact->ly;
	switch (contact->role) {
	case Role::client:
		wlr_seat_touch_notify_motion(server_.seat, time, contact->id,
			contact->sx + (lx - contact->lx0) * contact->scale,
			contact->sy + (ly - contact->ly0) * contact->scale);
		break;
	case Role::pointer: {
		Press *press = std::get_if<Press>(&state_);
		if (press == nullptr) {
			break;
		}
		if (press->phase == Press::Phase::waiting) {
			if (std::hypot(lx - contact->lx0, ly - contact->ly0) <= config::drag_threshold) {
				break;
			}
			// A drag: the press lands where the finger did, then follows it.
			if (hold_timer_) {
				wl_event_source_timer_update(hold_timer_.get(), 0);
			}
			press->phase = Press::Phase::pressed;
			input_.router.contact_button(time, BTN_LEFT, true);
		}
		if (press->phase == Press::Phase::pressed) {
			input_.router.contact_motion(time, lx, ly);
		}
		break;
	}
	case Role::view:
		// Read at the frame, once every finger in it has moved.
		moved_ = true;
		moved_time_ = time;
		break;
	case Role::ignored:
		break;
	}
}

void Touch::handle_up(wlr_touch_up_event *event) {
	if (input_.x_gate.waits<&Touch::handle_up>(this, event, false)) {
		return;
	}
	auto it = std::ranges::find(contacts_, event->touch_id, &Contact::id);
	if (it == contacts_.end()) {
		return;
	}
	input_.contact_activity();
	uint32_t time = event->time_msec;
	apply_moves(); // this frame's moves count before a gesture ends
	Contact contact = *it;
	contacts_.erase(it);
	switch (contact.role) {
	case Role::client:
		wlr_seat_touch_notify_up(server_.seat, time, contact.id);
		break;
	case Role::pointer: {
		const Press *press = std::get_if<Press>(&state_);
		bool tap = press != nullptr && press->phase == Press::Phase::waiting;
		end_press(time);
		if (tap) {
			// A tap: a click where the finger landed.
			input_.router.contact_button(time, BTN_LEFT, true);
			input_.router.contact_button(time, BTN_LEFT, false);
		}
		break;
	}
	case Role::view:
		// A finger lifting ends the gesture it was one of, as on the touchpad.
		end_swipe(time, false);
		end_two(time, false);
		break;
	case Role::ignored:
		break;
	}
}

void Touch::handle_cancel(wlr_touch_cancel_event *event) {
	if (input_.x_gate.waits<&Touch::handle_cancel>(this, event, false)) {
		return;
	}
	auto it = std::ranges::find(contacts_, event->touch_id, &Contact::id);
	if (it == contacts_.end()) {
		return;
	}
	uint32_t time = event->time_msec;
	Contact contact = *it;
	switch (contact.role) {
	case Role::client:
		if (wlr_touch_point *point = wlr_seat_touch_get_point(server_.seat, contact.id);
				point != nullptr && point->client != nullptr) {
			wlr_seat_touch_notify_cancel(server_.seat, point->client);
		}
		break;
	case Role::pointer:
		end_press(time);
		break;
	case Role::view:
		end_swipe(time, true);
		end_two(time, true);
		break;
	case Role::ignored:
		break;
	}
	// A cancel for a client is for all of its fingers.
	std::erase_if(contacts_, [this, &contact](const Contact &c) {
		return c.id == contact.id || (c.role == Role::client
			&& wlr_seat_touch_get_point(server_.seat, c.id) == nullptr);
	});
}

void Touch::handle_frame() {
	if (input_.x_gate.waits<&Touch::handle_frame>(this)) {
		return;
	}
	apply_moves();
	wlr_seat_touch_notify_frame(server_.seat);
}

void Touch::apply_moves() {
	if (!moved_) {
		return;
	}
	moved_ = false;
	if (Swipe *swipe = std::get_if<Swipe>(&state_)) {
		update_swipe(*swipe, moved_time_);
	} else if (TwoFinger *two = std::get_if<TwoFinger>(&state_)) {
		update_two(*two, moved_time_);
	}
}

// ---- gestures ----

void Touch::begin_swipe(uint32_t time, uint32_t fingers) {
	// Three or four fingers are the compositor's: what had reached a client
	// is cancelled for it, a press is let go, a two-finger move ends.
	cancel_clients();
	end_press(time);
	end_two(time, true);
	for (Contact &contact : contacts_) {
		contact.role = Role::view;
	}
	Swipe swipe{.fingers = fingers};
	centroid(swipe.cx, swipe.cy);
	state_ = swipe;
	// The gestures act on the output under the cursor: it goes between the
	// fingers.
	wlr_cursor_warp_closest(server_.cursor, nullptr, swipe.cx, swipe.cy);
	server_.gestures.swipe_begin(time, fingers);
}

void Touch::update_swipe(Swipe &swipe, uint32_t time) {
	double cx = 0, cy = 0;
	if (!centroid(cx, cy)) {
		return;
	}
	double dx = cx - swipe.cx, dy = cy - swipe.cy;
	swipe.cx = cx;
	swipe.cy = cy;
	swipe.travel += std::hypot(dx, dy);
	server_.gestures.swipe_update(time, dx, dy);
}

void Touch::update_two(TwoFinger &two, uint32_t time) {
	const Contact *a = nullptr, *b = nullptr;
	for (const Contact &contact : contacts_) {
		if (contact.role != Role::view) {
			continue;
		}
		if (a == nullptr) {
			a = &contact;
		} else {
			b = &contact;
			break;
		}
	}
	Output *output = server_.output_at(two.cx0, two.cy0);
	if (a == nullptr || b == nullptr || output == nullptr) {
		return;
	}
	double cx = (a->lx + b->lx) / 2, cy = (a->ly + b->ly) / 2;
	double dist = std::hypot(a->lx - b->lx, a->ly - b->ly);
	if (two.phase == TwoFinger::Phase::undecided) {
		if (two.dist0 > 0 && std::abs(dist / two.dist0 - 1) > config::touch_pinch_ratio) {
			two.phase = TwoFinger::Phase::pinching;
			// The pinch acts under the cursor: it goes between the fingers.
			wlr_cursor_warp_closest(server_.cursor, nullptr, cx, cy);
			server_.gestures.pinch_begin(time, 2, true);
		} else if (std::hypot(cx - two.cx0, cy - two.cy0) > config::drag_threshold) {
			two.phase = TwoFinger::Phase::looking; // the whole travel so far counts
		} else {
			return;
		}
	}
	if (two.phase == TwoFinger::Phase::looking) {
		// The content follows the fingers.
		input_.router.look_away(*output, two.cx - cx, two.cy - cy);
	} else {
		server_.gestures.pinch_update(time, cx - two.cx, cy - two.cy, dist / two.dist0, 0);
	}
	two.cx = cx;
	two.cy = cy;
}

void Touch::end_swipe(uint32_t time, bool cancelled) {
	if (!std::holds_alternative<Swipe>(state_)) {
		return;
	}
	state_ = Idle{};
	server_.gestures.swipe_end(time, cancelled);
	for (Contact &contact : contacts_) {
		contact.role = Role::ignored;
	}
}

void Touch::end_two(uint32_t time, bool cancelled) {
	const TwoFinger *two = std::get_if<TwoFinger>(&state_);
	if (two == nullptr) {
		return;
	}
	if (two->phase == TwoFinger::Phase::pinching) {
		server_.gestures.pinch_end(time, cancelled);
	}
	for (Contact &contact : contacts_) {
		if (contact.role == Role::view) {
			contact.role = Role::ignored;
		}
	}
	state_ = Idle{};
}

// ---- endings ----

void Touch::cancel_clients() {
	for (Contact &contact : contacts_) {
		if (contact.role != Role::client) {
			continue;
		}
		if (wlr_touch_point *point = wlr_seat_touch_get_point(server_.seat, contact.id);
				point != nullptr && point->client != nullptr) {
			wlr_seat_touch_notify_cancel(server_.seat, point->client);
		}
		contact.role = Role::ignored;
	}
}

void Touch::end_press(uint32_t time) {
	if (hold_timer_) {
		wl_event_source_timer_update(hold_timer_.get(), 0);
	}
	const Press *press = std::get_if<Press>(&state_);
	if (press == nullptr) {
		return;
	}
	if (press->phase == Press::Phase::pressed) {
		input_.router.contact_button(time, BTN_LEFT, false);
	}
	state_ = Idle{};
}

void Touch::end_all(uint32_t time) {
	cancel_clients();
	end_press(time);
	end_swipe(time, true);
	end_two(time, true);
	contacts_.clear();
}

// ---- press and hold ----

void Touch::arm_hold() {
	if (!hold_timer_) {
		hold_timer_.reset(wl_event_loop_add_timer(server_.loop, handle_hold_timer, this));
	}
	if (hold_timer_) {
		wl_event_source_timer_update(hold_timer_.get(), config::touch_hold_ms);
	}
}

int Touch::handle_hold_timer(void *data) {
	static_cast<Touch *>(data)->held();
	return 0;
}

void Touch::held() {
	Press *press = std::get_if<Press>(&state_);
	if (press == nullptr || press->phase != Press::Phase::waiting) {
		return;
	}
	// Held still: a right click where the finger landed; its lift does nothing.
	press->phase = Press::Phase::held;
	uint32_t time = static_cast<uint32_t>(now_ms());
	input_.router.contact_button(time, BTN_RIGHT, true);
	input_.router.contact_button(time, BTN_RIGHT, false);
}
