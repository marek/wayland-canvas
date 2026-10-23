#include "windows/placement.h"

#include <algorithm>
#include <cmath>
#include <format>
#include "core/box.h"
#include "platform/output.h"
#include "core/server.h"

namespace {

// `area` shown at `zoom`, less the output's bars; the whole area when the
// bars would leave nothing.
wlr_box inset_usable(const Output *output, wlr_box area, double zoom) {
	if (output == nullptr) {
		return area;
	}
	wlr_box inset = output->usable_canvas_box(area, zoom);
	if (inset.width < 1 || inset.height < 1) {
		return area;
	}
	return inset;
}

// The viewport of the output showing the view's frame centre in normal mode,
// else its desktop box, else a screen-sized box centred on the frame.
wlr_box maximize_area(View &view) {
	Rect frame = view.frame_rect();
	Output *picked = view.server.output_showing(frame.cx(), frame.cy(),
		Server::Showing::normal_view);
	if (picked != nullptr) {
		return inset_usable(picked, box_of_viewport(picked->viewport()),
			picked->camera.drawn().zoom);
	}
	Desktop *desktop = view.server.canvas.desktop_of(view);
	if (desktop != nullptr) {
		return inset_usable(view.server.output_under_cursor(), desktop->frame(),
			desktop->scale());
	}
	Output *any = view.server.output_under_cursor();
	int w = any != nullptr ? any->width : 1280;
	int h = any != nullptr ? any->height : 720;
	wlr_box area{
		static_cast<int>(std::round(frame.cx())) - w / 2,
		static_cast<int>(std::round(frame.cy())) - h / 2,
		w, h,
	};
	return inset_usable(any, area, 1.0); // off every desktop the camera rests at 1
}

// The screen-sized canvas box a new window opens centred in, the zoom its
// view rests at, and the output whose bars inset it.
struct Opening {
	Output *output;
	wlr_box box;
	double zoom;
};

// The spot a launch from the overview was made for, else the open
// overview's target, else the viewport of the output under the cursor.
Opening opening_of(const View &view, Output &under_cursor) {
	if (!view.launched_for.has_value()) {
		Overview &overview = under_cursor.overview;
		if (overview.active()) {
			return {&under_cursor, overview.target().box, overview.target_camera().zoom};
		}
		return {&under_cursor, box_of_viewport(under_cursor.viewport()),
			under_cursor.camera.drawn().zoom};
	}
	const LaunchSpot &spot = *view.launched_for;
	Output *output = &under_cursor;
	for (const std::unique_ptr<Output> &candidate : view.server.outputs) {
		if (spot.output == candidate->wlr->name) {
			output = candidate.get();
			break;
		}
	}
	// The desktop may have moved or zoomed since the launch: its frame now.
	const Desktop *desktop = view.server.canvas.desktops.find(spot.target.desktop);
	if (desktop != nullptr) {
		return {output, desktop->frame(), desktop->scale()};
	}
	return {output, spot.target.box, spot.zoom};
}

// A dialog sits over the window it belongs to, its title bar never above
// that window's.
void place_over_parent(View &view, const View &parent) {
	wlr_box frame = view.frame_box();
	int tb = view.deco.titlebar();
	int bw = view.deco.border();
	wlr_box over = parent.frame_box();
	double px = over.x + (over.width - frame.width) / 2.0;
	double py = std::fmax(over.y, over.y + (over.height - frame.height) / 2.0);
	view.x = static_cast<int>(std::round(px)) + bw;
	view.y = static_cast<int>(std::round(py)) + tb + bw;
}

// Opens the window with its frame at `frame`'s corner, stepped by
// config::cascade_step while another view occupies the spot, as far as the
// frame stays inside `area`; with every position that far taken, on the
// first again.
void cascade(View &view, const Rect &area, wlr_box frame) {
	int x = frame.x + view.deco.border();
	int y = frame.y + view.deco.titlebar() + view.deco.border();
	auto taken = [&](int step) {
		for (View *other : view.server.views) {
			if (other != &view && other->mapped
					&& other->x == x + config::cascade_step * step
					&& other->y == y + config::cascade_step * step) {
				return true;
			}
		}
		return false;
	};
	double room = std::fmin(area.x + area.w - (frame.x + frame.width),
		area.y + area.h - (frame.y + frame.height));
	int last = room > 0 ? static_cast<int>(room) / config::cascade_step : 0;
	int step = 0;
	while (step <= last && taken(step)) {
		step++;
	}
	if (step > last) {
		step = 0;
	}
	view.x = x + config::cascade_step * step;
	view.y = y + config::cascade_step * step;
}

// Centres the window in the opening's screen minus bars (exclusive zones), at
// the zoom the view rests at there, its frame kept inside it.
void centre_in(View &view, const Opening &opening) {
	wlr_box frame = view.frame_box();
	Rect area = rect_of_box(opening.output->usable_canvas_box(opening.box, opening.zoom));
	double px = area.x + (area.w - frame.width) / 2.0;
	double py = area.y + (area.h - frame.height) / 2.0;
	if (frame.width > area.w) {
		px = area.x;
	}
	if (frame.height > area.h) {
		py = area.y;
	}
	px = clampd(px, area.x, std::fmax(area.x, area.x + area.w - frame.width));
	py = clampd(py, area.y, std::fmax(area.y, area.y + area.h - frame.height));
	frame.x = static_cast<int>(std::round(px));
	frame.y = static_cast<int>(std::round(py));
	cascade(view, area, frame);
}

} // namespace

// Fullscreen covers the whole output viewport, no decorations, no inset; a
// window on a desktop of its own covers that desktop.
wlr_box fullscreen_area(View &view) {
	if (std::optional<wlr_box> own = view.server.canvas.fullscreen_desktops.area(view)) {
		return *own;
	}
	Rect frame = view.frame_rect();
	Output *picked = view.server.output_showing(frame.cx(), frame.cy());
	if (picked == nullptr) {
		picked = view.server.output_under_cursor();
	}
	if (picked == nullptr) {
		return wlr_box{
			static_cast<int>(std::round(frame.cx())) - 640,
			static_cast<int>(std::round(frame.cy())) - 360,
			1280, 720,
		};
	}
	return box_of_viewport(picked->viewport());
}

std::optional<wlr_box> area_for(View &view, WindowMode mode) {
	if (mode == WindowMode::maximized) {
		return maximize_area(view);
	}
	if (mode == WindowMode::fullscreen) {
		return fullscreen_area(view);
	}
	return std::nullopt;
}

bool shown_anywhere(const View &view) {
	Rect frame = view.frame_rect();
	return view.server.output_showing(frame.cx(), frame.cy()) != nullptr
		|| std::ranges::any_of(view.server.outputs,
			[](const std::unique_ptr<Output> &output) { return output->overview.active(); });
}

std::string opened_where(View &view) {
	const Desktop *desktop = view.server.canvas.desktop_of(view);
	if (desktop == nullptr) {
		return "Opened in a free area";
	}
	return std::format("Opened in Desktop {}", desktop->number());
}

void place(View &view) {
	Output *output = view.server.output_under_cursor();
	if (output == nullptr) {
		view.x = 0;
		view.y = 0;
		return;
	}
	if (std::optional<wlr_box> area = area_for(view, view.mode)) {
		view.x = area->x;
		view.y = area->y + view.deco.titlebar();
		return;
	}
	View *parent = opens_over_parent(view.kind()) ? view.shell_parent() : nullptr;
	if (parent != nullptr && parent->mapped && !parent->minimized) {
		place_over_parent(view, *parent);
		return;
	}
	centre_in(view, opening_of(view, *output));
}
