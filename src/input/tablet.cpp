// tablet.cpp: the pen (docs/intents/input/touch-and-pen.md). A tool goes to
// the client under it through tablet-v2 when that client has bound the
// tablet; anywhere else it drives the pointer through the mouse's own
// handling.
#include "input/tablet.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include "core/server.h"
#include "core/process.h"
#include "input/input.h"
#include "platform/output.h"

Tablets::Device::Device(Tablets &tablets, wlr_input_device *device)
	: wlr(device), tablets_(tablets) {
	// Ours before tablet-v2's own, so its twin is still there when this goes.
	destroy_.connect<&Device::handle_destroy>(device->events.destroy, this);
	v2 = wlr_tablet_create(tablets.manager_, tablets.server_.seat, device);
}

void Tablets::Device::handle_destroy() {
	tablets_.device_gone(this);
}

Tablets::Tool::Tool(Tablets &tablets, wlr_tablet_tool *wlr)
	: wlr(wlr), tablets_(tablets) {
	// Ours before tablet-v2's own: the set_cursor listener comes off its
	// twin before the twin is freed.
	destroy_.connect<&Tool::handle_destroy>(wlr->events.destroy, this);
	v2 = wlr_tablet_tool_create(tablets.manager_, tablets.server_.seat, wlr);
	if (v2 != nullptr) {
		set_cursor_.connect<&Tool::handle_set_cursor>(v2->events.set_cursor, this);
	}
}

void Tablets::Tool::handle_destroy() {
	set_cursor_.disconnect();
	tablets_.tool_gone(this);
}

// The client the tool is in range of draws the tool's cursor.
void Tablets::Tool::handle_set_cursor(wlr_tablet_v2_event_cursor *event) {
	wlr_surface *focus = v2->focused_surface;
	if (focus == nullptr || event->seat_client == nullptr
			|| event->seat_client->client != wl_resource_get_client(focus->resource)) {
		return;
	}
	wlr_cursor_set_surface(tablets_.server_.cursor, event->surface, event->hotspot_x,
		event->hotspot_y);
}

Tablets::Tablets(Server &server, Input &input) : server_(server), input_(input) {}

Tablets::~Tablets() = default;

void Tablets::init() {
	manager_ = wlr_tablet_v2_create(server_.display);
	wlr_cursor *cursor = server_.cursor;
	axis_.connect<&Tablets::handle_axis>(cursor->events.tablet_tool_axis, this);
	proximity_.connect<&Tablets::handle_proximity>(cursor->events.tablet_tool_proximity, this);
	tip_.connect<&Tablets::handle_tip>(cursor->events.tablet_tool_tip, this);
	button_.connect<&Tablets::handle_button>(cursor->events.tablet_tool_button, this);
}

void Tablets::finish() {
	axis_.disconnect();
	proximity_.disconnect();
	tip_.disconnect();
	button_.disconnect();
	tools_.clear();
	devices_.clear();
}

void Tablets::new_device(wlr_input_device *device) {
	wlr_cursor_attach_input_device(server_.cursor, device);
	devices_.push_back(std::make_unique<Device>(*this, device));
	map_devices();
}

// A tablet names no output of its own: the built-in panel, else the first.
void Tablets::map_devices() {
	Output *output = input_.output_for_device(nullptr);
	for (const std::unique_ptr<Device> &device : devices_) {
		wlr_cursor_map_input_to_output(server_.cursor, device->wlr,
			output != nullptr ? output->wlr : nullptr);
	}
}

void Tablets::device_gone(Device *device) {
	input_.x_gate.run_waiting(); // a waiting event names this tablet
	// The pen leaves what it is on while the tablet's twin is still there.
	end_all(static_cast<uint32_t>(now_ms()));
	destroy_owned(devices_, device);
}

void Tablets::tool_gone(Tool *tool) {
	input_.x_gate.run_waiting(); // a waiting event names this tool
	if (tool->tip_as_button) {
		input_.router.contact_button(static_cast<uint32_t>(now_ms()), BTN_LEFT, false);
	}
	destroy_owned(tools_, tool);
}

Tablets::Device *Tablets::device_for(wlr_tablet *tablet) {
	auto it = std::ranges::find_if(devices_, [tablet](const std::unique_ptr<Device> &d) {
		return d->wlr == &tablet->base;
	});
	return it != devices_.end() ? it->get() : nullptr;
}

Tablets::Tool *Tablets::tool_for(wlr_tablet_tool *wlr) {
	auto it = std::ranges::find_if(tools_, [wlr](const std::unique_ptr<Tool> &t) {
		return t->wlr == wlr;
	});
	if (it != tools_.end()) {
		return it->get();
	}
	tools_.push_back(std::make_unique<Tool>(*this, wlr));
	return tools_.back().get();
}

void Tablets::move(Tool &tool, Device &device, uint32_t time) {
	double lx = server_.cursor->x, ly = server_.cursor->y;
	wlr_tablet_v2_tablet_tool *v2 = tool.v2;
	// The tip down on a client keeps the tool there, past its edges too.
	if (v2->focused_surface != nullptr && wlr_tablet_tool_v2_has_implicit_grab(v2)) {
		wlr_tablet_v2_tablet_tool_notify_motion(v2, tool.sx0 + (lx - tool.lx0) * tool.scale,
			tool.sy0 + (ly - tool.ly0) * tool.scale);
		return;
	}
	// The tip down as the mouse's button stays the mouse until it lifts.
	std::optional<PointerRouter::ContactHit> hit;
	if (!tool.tip_as_button) {
		hit = input_.router.client_at(lx, ly);
	}
	if (hit.has_value() && device.v2 != nullptr
			&& wlr_surface_accepts_tablet_v2(hit->surface, device.v2)) {
		if (v2->focused_surface != hit->surface) {
			if (v2->focused_surface != nullptr) {
				wlr_tablet_v2_tablet_tool_notify_proximity_out(v2);
			}
			wlr_tablet_v2_tablet_tool_notify_proximity_in(v2, device.v2, hit->surface);
			// The tool is the client's pointer now: the mouse's focus leaves.
			wlr_seat_pointer_notify_clear_focus(server_.seat);
		}
		tool.sx0 = hit->sx;
		tool.sy0 = hit->sy;
		tool.lx0 = lx;
		tool.ly0 = ly;
		tool.scale = hit->scale;
		wlr_tablet_v2_tablet_tool_notify_motion(v2, hit->sx, hit->sy);
		return;
	}
	if (v2->focused_surface != nullptr) {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(v2);
	}
	input_.router.contact_motion(time, lx, ly);
}

void Tablets::send_axes(Tool &tool, const wlr_tablet_tool_axis_event &event) {
	wlr_tablet_v2_tablet_tool *v2 = tool.v2;
	if (event.updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) {
		wlr_tablet_v2_tablet_tool_notify_pressure(v2, event.pressure);
	}
	if (event.updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) {
		wlr_tablet_v2_tablet_tool_notify_distance(v2, event.distance);
	}
	if (event.updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) {
		if (event.updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X) {
			tool.tilt_x = event.tilt_x;
		}
		if (event.updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y) {
			tool.tilt_y = event.tilt_y;
		}
		wlr_tablet_v2_tablet_tool_notify_tilt(v2, tool.tilt_x, tool.tilt_y);
	}
	if (event.updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION) {
		wlr_tablet_v2_tablet_tool_notify_rotation(v2, event.rotation);
	}
	if (event.updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER) {
		wlr_tablet_v2_tablet_tool_notify_slider(v2, event.slider);
	}
	if (event.updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) {
		wlr_tablet_v2_tablet_tool_notify_wheel(v2, event.wheel_delta, 0);
	}
}

void Tablets::handle_proximity(wlr_tablet_tool_proximity_event *event) {
	if (input_.x_gate.waits<&Tablets::handle_proximity>(this, event, false)) {
		return;
	}
	input_.contact_activity();
	Device *device = device_for(event->tablet);
	Tool *tool = tool_for(event->tool);
	if (device == nullptr || tool->v2 == nullptr) {
		return;
	}
	if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
		wlr_cursor_warp_absolute(server_.cursor, &event->tablet->base, event->x, event->y);
		move(*tool, *device, event->time_msec);
		return;
	}
	// Out of range: a tip still down lifts first, so no press stays held.
	leave(*tool, event->time_msec);
}

void Tablets::leave(Tool &tool, uint32_t time) {
	if (tool.tip_as_button) {
		tool.tip_as_button = false;
		input_.router.contact_button(time, BTN_LEFT, false);
	}
	if (tool.v2 != nullptr && tool.v2->focused_surface != nullptr) {
		if (tool.v2->is_down) {
			wlr_tablet_v2_tablet_tool_notify_up(tool.v2);
		}
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool.v2);
	}
}

void Tablets::end_all(uint32_t time) {
	for (const std::unique_ptr<Tool> &tool : tools_) {
		leave(*tool, time);
	}
}

void Tablets::handle_axis(wlr_tablet_tool_axis_event *event) {
	if (input_.x_gate.waits<&Tablets::handle_axis>(this, event, false)) {
		return;
	}
	input_.contact_activity();
	Device *device = device_for(event->tablet);
	Tool *tool = tool_for(event->tool);
	if (device == nullptr || tool->v2 == nullptr) {
		return;
	}
	if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
		if (event->tool->type == WLR_TABLET_TOOL_TYPE_MOUSE
				|| event->tool->type == WLR_TABLET_TOOL_TYPE_LENS) {
			wlr_cursor_move(server_.cursor, &event->tablet->base, event->dx, event->dy);
		} else {
			wlr_cursor_warp_absolute(server_.cursor, &event->tablet->base,
				(event->updated_axes & WLR_TABLET_TOOL_AXIS_X) ? event->x : NAN,
				(event->updated_axes & WLR_TABLET_TOOL_AXIS_Y) ? event->y : NAN);
		}
		move(*tool, *device, event->time_msec);
	}
	if (tool->v2->focused_surface != nullptr) {
		send_axes(*tool, *event);
	}
}

void Tablets::handle_tip(wlr_tablet_tool_tip_event *event) {
	Tool *tool = tool_for(event->tool);
	// The tip down on an X11 window waits until X has taken in its window moves.
	if (input_.x_gate.waits<&Tablets::handle_tip>(this, event,
			event->state == WLR_TABLET_TOOL_TIP_DOWN && tool->v2 != nullptr
			&& Xwm::is_x11_window(tool->v2->focused_surface))) {
		return;
	}
	input_.contact_activity();
	if (tool->v2 == nullptr) {
		return;
	}
	if (event->state == WLR_TABLET_TOOL_TIP_DOWN) {
		if (tool->v2->focused_surface != nullptr) {
			// The tip down is a click: its window is focused and raised.
			if (std::optional<PointerRouter::ContactHit> hit =
					input_.router.client_at(server_.cursor->x, server_.cursor->y)) {
				input_.router.contact_focus(*hit);
			}
			wlr_tablet_v2_tablet_tool_notify_down(tool->v2);
			wlr_tablet_tool_v2_start_implicit_grab(tool->v2);
		} else {
			tool->tip_as_button = true;
			input_.router.contact_button(event->time_msec, BTN_LEFT, true);
		}
		return;
	}
	if (tool->v2->is_down) {
		wlr_tablet_v2_tablet_tool_notify_up(tool->v2);
	} else if (tool->tip_as_button) {
		tool->tip_as_button = false;
		input_.router.contact_button(event->time_msec, BTN_LEFT, false);
	}
}

void Tablets::handle_button(wlr_tablet_tool_button_event *event) {
	Tool *tool = tool_for(event->tool);
	if (input_.x_gate.waits<&Tablets::handle_button>(this, event,
			event->state == WLR_BUTTON_PRESSED && tool->v2 != nullptr
			&& Xwm::is_x11_window(tool->v2->focused_surface))) {
		return;
	}
	input_.contact_activity();
	if (tool->v2 == nullptr) {
		return;
	}
	bool pressed = event->state == WLR_BUTTON_PRESSED;
	if (tool->v2->focused_surface != nullptr) {
		wlr_tablet_v2_tablet_tool_notify_button(tool->v2, event->button, pressed
			? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
		return;
	}
	// As the mouse: the barrel button is the right button, the second the middle.
	uint32_t button = event->button == BTN_STYLUS ? BTN_RIGHT
		: event->button == BTN_STYLUS2 ? BTN_MIDDLE : 0;
	if (button != 0) {
		input_.router.contact_button(event->time_msec, button, pressed);
	}
}
