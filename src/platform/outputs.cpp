#include "platform/outputs.h"

#include <cmath>
#include <cstdint>
#include <memory>

#include "core/handle.h"
#include "core/server.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

double pick_scale(const Server &server, wlr_output *wlr, wlr_output_mode *mode) {
	if (server.options.scale > 0) {
		return server.options.scale;
	}
	int width = mode != nullptr ? mode->width : wlr->width;
	int height = mode != nullptr ? mode->height : wlr->height;
	if (wlr->phys_width <= 0 || wlr->phys_height <= 0) {
		return 1;
	}
	double dpi_x = width / (wlr->phys_width / 25.4);
	double dpi_y = height / (wlr->phys_height / 25.4);
	// GNOME's heuristic: 2x only for dense panels that stay large enough.
	return std::fmin(dpi_x, dpi_y) >= 192 && height >= 1200 ? 2 : 1;
}

void update_output_geometry(Output &output) {
	wlr_box box{};
	wlr_output_layout_get_box(output.server.output_layout, output.wlr, &box);
	if (wlr_box_empty(&box)) {
		return;
	}
	bool resized = box.width != output.width || box.height != output.height;
	output.lx = box.x;
	output.ly = box.y;
	output.width = box.width;
	output.height = box.height;
	if (resized) {
		output.usable = wlr_box{0, 0, box.width, box.height};
		output.arrange_layers();
		output.server.lock.output_changed(output);
	}
	output.damage();
}

} // namespace

Outputs::Outputs(Server &server) : server_(server) {}

Outputs::~Outputs() = default;

void Outputs::init() {
	new_output_.connect<&Outputs::handle_new_output>(server_.backend->events.new_output, this);
	layout_change_.connect<&Outputs::handle_layout_change>(
		server_.output_layout->events.change, this);
}

void Outputs::finish() {
	new_output_.disconnect();
	layout_change_.disconnect();
}

void Outputs::erase(const Output &output) {
	destroy_owned(list_, &output);
}

void Outputs::damage_all() {
	for (const std::unique_ptr<Output> &output : list_) {
		output->damage();
	}
}

Output *Outputs::at(double lx, double ly) const {
	wlr_output *wlr = wlr_output_layout_output_at(server_.output_layout, lx, ly);
	return wlr != nullptr ? static_cast<Output *>(wlr->data) : nullptr;
}

Output *Outputs::under_cursor() const {
	Output *output = at(server_.cursor->x, server_.cursor->y);
	if (output == nullptr && !list_.empty()) {
		output = list_.front().get();
	}
	return output;
}

Output *Outputs::showing(double cx, double cy, Showing which) const {
	for (const std::unique_ptr<Output> &output : list_) {
		if (which == Showing::normal_view && output->overview.active()) {
			continue;
		}
		if (output->viewport().contains(cx, cy)) {
			return output.get();
		}
	}
	return nullptr;
}

void Outputs::update_visibility() {
	if (!visibility_dirty_) {
		return;
	}
	visibility_dirty_ = false;
	for (View *view : server_.views) {
		uint32_t mask = 0;
		if (view->mapped && !view->minimized) {
			Rect frame = view->frame_rect();
			for (const std::unique_ptr<Output> &output : list_) {
				if (frame.intersects(output->viewport())) {
					mask |= output->bit;
				}
			}
		}
		view->update_outputs(mask);
	}
}

uint32_t Outputs::free_bit() const {
	uint32_t used = 0;
	for (const std::unique_ptr<Output> &output : list_) {
		used |= output->bit;
	}
	// The lowest clear bit of `used` is the lowest set bit of `used + 1`,
	// which wraps to 0 once all 32 are taken.
	return ~used & (used + 1);
}

void Outputs::handle_new_output(wlr_output *wlr) {
	if (wlr->non_desktop) {
		return;
	}
	uint32_t bit = free_bit();
	if (bit == 0) {
		wlr_log(WLR_ERROR, "too many outputs, ignoring %s", wlr->name);
		return;
	}
	wlr_output_init_render(wlr, server_.allocator, server_.renderer);

	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
	if (mode != nullptr) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_state_set_scale(&state, pick_scale(server_, wlr, mode));
	if (!wlr_output_commit_state(wlr, &state)) {
		wlr_log(WLR_ERROR, "failed to enable %s with its preferred mode", wlr->name);
		wlr_output_state_finish(&state);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_commit_state(wlr, &state);
	}
	wlr_output_state_finish(&state);

	list_.push_back(std::make_unique<Output>(server_, wlr, bit));
	Output &output = *list_.back();

	wlr_output_layout_add_auto(server_.output_layout, wlr);
	update_output_geometry(output);
	server_.outputs_changed();
	// The canvas starts out aligned with the output layout.
	output.camera.jump(Camera{
		.cx = output.lx + output.width / 2.0,
		.cy = output.ly + output.height / 2.0,
		.zoom = 1,
	});
	if (server_.canvas.desktops.empty()) {
		server_.canvas.desktops.create(
			wlr_box{output.lx, output.ly, output.width, output.height});
	}
	wlr_log(WLR_INFO, "output %s: %dx%d logical at scale %.2f", wlr->name,
		output.width, output.height, wlr->scale);
	output.visibility_changed();
}

void Outputs::handle_layout_change() {
	for (const std::unique_ptr<Output> &output : list_) {
		update_output_geometry(*output);
	}
	visibility_changed();
	server_.changed(Change::outputs); // a moved or resized output changes the state
}
