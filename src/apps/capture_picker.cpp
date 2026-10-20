#include "apps/capture_picker.h"

#include <linux/input-event-codes.h>

#include <cmath>
#include <format>

#include "apps/capture_geometry.h"
#include "core/config.h"
#include "core/server.h"
#include "core/theme.h"
#include "platform/output.h"
#include "render/draw.h"
#include "windows/view.h"

namespace {

wlr_box normalized_box(double x0, double y0, double x1, double y1) {
	return {
		static_cast<int>(std::round(std::fmin(x0, x1))),
		static_cast<int>(std::round(std::fmin(y0, y1))),
		static_cast<int>(std::round(std::fabs(x1 - x0))),
		static_cast<int>(std::round(std::fabs(y1 - y0))),
	};
}

// Four dim fills around `box`, covering the rest of the output.
void dim_around(CaptureOverlay &out, const wlr_box &box, const Output &output,
		uint32_t rgba) {
	out.fills.push_back({.box = {0, 0, output.width, box.y}, .rgba = rgba});
	out.fills.push_back({.box = {0, box.y + box.height, output.width,
		output.height - box.y - box.height}, .rgba = rgba});
	out.fills.push_back({.box = {0, box.y, box.x, box.height}, .rgba = rgba});
	out.fills.push_back({.box = {box.x + box.width, box.y,
		output.width - box.x - box.width, box.height}, .rgba = rgba});
}

} // namespace

CapturePicker::CapturePicker(Server &server) : server_(server) {
}

bool CapturePicker::spans_outputs() const {
	return state_.what == PickWhat::any || state_.what == PickWhat::output;
}

bool CapturePicker::uses(const Output &output) const {
	return state_.active && (state_.output == &output || state_.cur_output == &output);
}

void CapturePicker::start(Output &output, PickWhat what, PickUse use,
		const CaptureOptions &options) {
	state_ = State{
		.active = true,
		.output = &output,
		.what = what,
		.window_mode = what == PickWhat::window || what == PickWhat::any,
		.use = use,
		.options = options,
	};
	state_.cur_output = &output;
	// The picker starts where the pointer already is: the crosshair and the
	// hover must not wait for the first motion to exist.
	state_.cur_x = server_.cursor->x - output.lx;
	state_.cur_y = server_.cursor->y - output.ly;
	state_.hover = window_at(output, state_.cur_x, state_.cur_y);
	server_.input.set_cursor_image(cursor_image());
	wlr_seat_pointer_notify_clear_focus(server_.seat);
	output.damage();
}

std::optional<PickUse> CapturePicker::close() {
	if (!state_.active) {
		return std::nullopt;
	}
	PickUse use = state_.use;
	Output *output = state_.output;
	state_ = State{};
	server_.input.set_cursor_image("default");
	if (output != nullptr) {
		output->damage();
	}
	return use;
}

bool CapturePicker::toggle_window_mode() {
	if (state_.what != PickWhat::region) {
		return false;
	}
	state_.window_mode = !state_.window_mode;
	server_.input.set_cursor_image(cursor_image());
	if (state_.output != nullptr) {
		state_.output->damage();
	}
	return true;
}

const char *CapturePicker::cursor_image() const {
	if (state_.active && !state_.window_mode && state_.what == PickWhat::region) {
		return "crosshair";
	}
	return "default";
}

View *CapturePicker::window_at(Output &output, double ox, double oy) const {
	Point p = output.local_to_canvas(ox, oy);
	for (View *view : server_.views) { // front is topmost
		if (view->mapped && !view->minimized && view->frame_rect().contains(p.x, p.y)) {
			return view;
		}
	}
	return nullptr;
}

void CapturePicker::forget(const View &view) {
	if (state_.hover == &view) {
		state_.hover = nullptr;
	}
}

bool CapturePicker::takes_pointer_at(const Output &output) const {
	return &output == state_.output || spans_outputs();
}

bool CapturePicker::pointer_motion(Output &output, double ox, double oy) {
	// Region picking stays on its own output; window and output picking
	// follow the pointer onto the other screens too.
	if (&output != state_.output && !spans_outputs()) {
		return false;
	}
	state_.cur_output = &output;
	state_.cur_x = ox;
	state_.cur_y = oy;
	if (state_.pressed && !state_.window_mode && state_.what == PickWhat::region) {
		double dx = ox - state_.press_x, dy = oy - state_.press_y;
		if (!state_.dragging && std::hypot(dx, dy) > config::drag_threshold) {
			state_.dragging = true;
		}
	}
	if (state_.window_mode) {
		View *hover = window_at(output, ox, oy);
		if (hover != state_.hover) {
			state_.hover = hover;
			output.damage();
		}
	}
	return true;
}

bool CapturePicker::pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed, Picked picked) {
	if (&output != state_.output && !spans_outputs()) {
		return false;
	}
	if (button != BTN_LEFT) {
		return true; // swallowed: the picker is modal
	}
	if (pressed) {
		state_.pressed = true;
		state_.dragging = false;
		state_.press_x = ox;
		state_.press_y = oy;
		return true;
	}
	state_.pressed = false;
	bool dragged = state_.dragging;
	state_.dragging = false;
	release(output, ox, oy, dragged, picked);
	return true;
}

// The release of a pick press: a drag takes its rectangle, a click takes
// the window or the output under the pointer.
void CapturePicker::release(Output &output, double ox, double oy, bool dragged,
		Picked picked) {
	if (dragged && &output == state_.output) {
		wlr_box area = normalized_box(state_.press_x, state_.press_y, ox, oy);
		if (area.width > 0 && area.height > 0) {
			complete(Pick{.kind = Pick::Kind::region, .output = state_.output,
				.area = area}, picked);
			return;
		}
		output.damage(); // a zero-sized drag picks nothing; keep picking
		return;
	}
	// A click: the window under the pointer, frame and all — every mode
	// but a plain output pick (Space mode and window picks included).
	if (state_.what != PickWhat::output) {
		if (View *view = window_at(output, ox, oy)) {
			complete(Pick{.kind = Pick::Kind::window, .output = &output,
				.view = view}, picked);
			return;
		}
		if (state_.what == PickWhat::any) {
			complete(Pick{.kind = Pick::Kind::output, .output = &output}, picked);
			return;
		}
		output.damage(); // no window there: the picker waits for a real pick
		return;
	}
	// Output picking: the screen the click landed on.
	complete(Pick{.kind = Pick::Kind::output, .output = &output}, picked);
}

void CapturePicker::complete(const Pick &pick, Picked picked) {
	PickUse use = state_.use;
	CaptureOptions options = state_.options;
	close();
	picked(pick, use, options);
}

// Everything outside the selection dimmed, a crosshair while a region is
// being framed, the selection with its size, the hovered window's frame, or
// the hovered output's own outline.
void CapturePicker::add_overlay(Output &output, float scale, CaptureOverlay &out) {
	const config::Palette &pal = server_.theme.colors();

	bool region = state_.what == PickWhat::region && !state_.window_mode;
	if (region) {
		if (&output != state_.output) {
			return; // a region is framed on the picker's own screen
		}
		wlr_box sel{};
		if (!state_.dragging) {
			int cx = static_cast<int>(std::round(state_.cur_x));
			int cy = static_cast<int>(std::round(state_.cur_y));
			out.fills.push_back({.box = {0, cy, output.width, 1},
				.rgba = pal.picker_cross});
			out.fills.push_back({.box = {cx, 0, 1, output.height},
				.rgba = pal.picker_cross});
			return;
		}
		sel = normalized_box(state_.press_x, state_.press_y, state_.cur_x,
			state_.cur_y);
		dim_around(out, sel, output, pal.launcher_backdrop);
		out.outlines.push_back({.box = sel, .rgba = pal.accent,
			.thickness = config::capture_pick_outline});
		// The size beside the selection's far corner, kept on the screen.
		std::string text = std::format("{} × {}", sel.width, sel.height);
		if (size_texture_ == nullptr || size_text_ != text || size_scale_ != scale) {
			size_texture_.reset();
			TextTexture label = draw_text(server_.renderer, text.c_str(),
				config::ui_font, pal.panel_text, pal.panel, scale, 10, 5);
			size_texture_ = std::move(label.texture);
			size_text_ = text;
			size_scale_ = scale;
			size_w_ = label.width;
			size_h_ = label.height;
		}
		if (size_texture_ != nullptr && size_w_ > 0) {
			int lx = sel.x + sel.width + config::capture_pick_label_gap;
			int ly = sel.y + sel.height + config::capture_pick_label_gap;
			if (lx + size_w_ > output.width) {
				lx = sel.x - config::capture_pick_label_gap - size_w_;
			}
			if (ly + size_h_ > output.height) {
				ly = sel.y - config::capture_pick_label_gap - size_h_;
			}
			out.images.push_back({.texture = size_texture_.get(),
				.box = {lx < 0 ? 0 : lx, ly < 0 ? 0 : ly, size_w_, size_h_}});
		}
		return;
	}

	// Window and output picking draw on the screen under the pointer.
	if (&output != state_.cur_output) {
		return;
	}
	bool windows = state_.window_mode || state_.what == PickWhat::window
		|| state_.what == PickWhat::any;
	if (windows && state_.hover != nullptr) {
		wlr_box frame = frame_on_output(output, *state_.hover);
		dim_around(out, frame, output, pal.launcher_backdrop);
		out.outlines.push_back({.box = frame, .rgba = pal.accent,
			.thickness = config::capture_pick_outline});
		return;
	}
	if (!windows || state_.what == PickWhat::any || state_.what == PickWhat::output) {
		// No window under the pointer: the screen itself is the candidate.
		int inset = static_cast<int>(config::capture_pick_outline);
		wlr_box whole{inset, inset, output.width - 2 * inset,
			output.height - 2 * inset};
		out.outlines.push_back({.box = whole, .rgba = pal.accent,
			.thickness = config::capture_pick_outline});
		return;
	}
	out.fills.push_back({.box = {0, 0, output.width, output.height},
		.rgba = pal.launcher_backdrop});
}

void CapturePicker::theme_changed() {
	size_texture_.reset();
	size_text_.clear();
	size_scale_ = 0;
}

void CapturePicker::finish() {
	size_texture_.reset();
}
