#include "output.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <vector>

#include "canvas.h"
#include "config.h"
#include "desktop.h"
#include "server.h"
#include "view.h"

namespace {

// Cap on views collected for tiling / combining; the rest stay untouched.
constexpr std::size_t box_views_max = 64;

Rect box_to_rect(wlr_box box) {
	return Rect{
		static_cast<double>(box.x),
		static_cast<double>(box.y),
		static_cast<double>(box.width),
		static_cast<double>(box.height),
	};
}

Point box_centre(wlr_box box) {
	return Point{
		.x = box.x + box.width / 2.0,
		.y = box.y + box.height / 2.0,
	};
}

// Screen-sized canvas box centred on (cx, cy), edges rounded to integers.
wlr_box centred_box(const Output &output, double cx, double cy) {
	int w = output.width, h = output.height;
	return wlr_box{
		static_cast<int>(std::round(cx - w / 2.0)),
		static_cast<int>(std::round(cy - h / 2.0)),
		w,
		h,
	};
}

} // namespace

Canvas::Canvas(Server &server)
	: desktops(server), server_(server) {
}

std::vector<View *> Canvas::views_in(wlr_box box) const {
	std::vector<View *> out;
	for (View *view : server_.views) {
		if (out.size() == box_views_max) {
			break;
		}
		if (!view->mapped || view->minimized) {
			continue;
		}
		Point centre = box_centre(view->frame_box());
		if (!wlr_box_contains_point(&box, centre.x, centre.y)) {
			continue;
		}
		out.push_back(view);
	}
	return out;
}

Desktop *Canvas::desktop_of(const View &view) {
	Point centre = box_centre(view.frame_box());
	return desktops.at(centre.x, centre.y);
}

void Canvas::tile(wlr_box area, const Output &output) {
	std::vector<View *> in_area = views_in(area);
	if (in_area.empty()) {
		server_.hint.set("No windows to tile here");
		return;
	}
	Rect usable = box_to_rect(output.usable_canvas_box(area));
	std::size_t n = in_area.size();
	auto cols = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
	std::size_t rows = (n + cols - 1) / cols;
	double band = usable.h / static_cast<double>(rows);
	std::vector<Rect> frames;
	frames.reserve(n);
	for (View *view : in_area) {
		frames.push_back(view->frame_rect());
	}
	std::vector<std::size_t> order = tile_order(frames, band);
	std::vector<Rect> cells = tile_layout(usable, n, config::tile_gap);
	for (std::size_t i = 0; i < n; i++) {
		wlr_box cell = {
			static_cast<int>(std::round(cells[i].x)),
			static_cast<int>(std::round(cells[i].y)),
			static_cast<int>(std::round(cells[i].w)),
			static_cast<int>(std::round(cells[i].h)),
		};
		in_area[order[i]]->set_frame(cell, true);
	}
	server_.hint.set(std::format("Tiled {} window{}", n, n == 1 ? "" : "s"));
}

void Canvas::combine(DesktopId into, DesktopId from, wlr_box from_area) {
	if (into == from) {
		return;
	}
	Desktop *into_desktop = desktops.find(into);
	Desktop *from_desktop = desktops.find(from);
	if (into_desktop == nullptr || from_desktop == nullptr) {
		return;
	}
	int dx = into_desktop->frame().x - from_area.x;
	int dy = into_desktop->frame().y - from_area.y;
	for (View *view : views_in(from_area)) {
		view->move(view->x + dx, view->y + dy);
		view->saved.x += dx;
		view->saved.y += dy;
	}
	int from_number = from_desktop->number(), into_number = into_desktop->number();
	desktops.remove(from);
	server_.hint.set(std::format("Combined Desktop {} into Desktop {}",
		from_number, into_number));
}

wlr_box Canvas::spot_at(double cx, double cy, const Output &output) {
	if (Desktop *desktop = desktops.at(cx, cy)) {
		return desktop->frame();
	}
	return centred_box(output, cx, cy);
}
