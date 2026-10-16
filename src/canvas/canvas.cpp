#include "platform/output.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <vector>

#include "canvas/canvas.h"
#include "core/config.h"
#include "canvas/desktop.h"
#include "core/server.h"
#include "windows/view.h"

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

// The views as a grid filling `usable`: cells with gaps, taken in reading
// order of where the windows stand, so a window dropped into a slot keeps it.
void flow_grid(std::span<View *const> views, const Rect &usable) {
	std::size_t n = views.size();
	auto cols = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
	std::size_t rows = (n + cols - 1) / cols;
	double band = usable.h / static_cast<double>(rows);
	std::vector<Rect> frames;
	frames.reserve(n);
	for (View *view : views) {
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
		views[order[i]]->set_frame(cell);
	}
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
	// A window a tiled desktop's grid holds is that desktop's wherever a zoom
	// has moved the frame's edges; any other is the desktop its centre is in.
	if (Desktop *holder = desktops.find(view.grid)) {
		return holder;
	}
	Point centre = box_centre(view.frame_box());
	return desktops.at(centre.x, centre.y);
}

bool Canvas::grid_member(const View &view, const Desktop &desktop) {
	if (!view.mapped || view.minimized || view.maximized || view.fullscreen) {
		return false;
	}
	return desktop_of(view) == &desktop;
}

std::vector<View *> Canvas::grid_members(const Desktop &desktop) {
	std::vector<View *> out;
	for (View *view : server_.views) {
		if (out.size() == box_views_max) {
			break;
		}
		if (grid_member(*view, desktop)) {
			out.push_back(view);
		}
	}
	return out;
}

Rect Canvas::tiling_area(const Desktop &desktop) {
	Point centre = desktop.centre();
	const Output *picked = server_.output_under_cursor();
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		if (output->viewport().contains(centre.x, centre.y)) {
			picked = output.get();
			break;
		}
	}
	if (picked == nullptr) {
		return desktop.rect();
	}
	return box_to_rect(picked->usable_canvas_box(desktop.frame(), desktop.scale()));
}

void Canvas::tile(wlr_box area, const Output &output) {
	std::vector<View *> in_area = views_in(area);
	if (in_area.empty()) {
		server_.hint.set("No windows to tile here");
		return;
	}
	flow_grid(in_area, box_to_rect(output.usable_canvas_box(area, 1.0))); // a free spot rests at 1
	server_.hint.set(std::format("Tiled {} window{}",
		in_area.size(), in_area.size() == 1 ? "" : "s"));
}

void Canvas::toggle_tiling(DesktopId id) {
	Desktop *desktop = desktops.find(id);
	if (desktop == nullptr) {
		return;
	}
	bool on = !desktop->tiling();
	desktops.set_tiling(id, on);
	if (on) {
		reflow(id);
		server_.hint.set(std::format("Tiling on for Desktop {}",
			desktop->number()));
	} else {
		// Windows stay where the grid left them, told they are no longer tiled.
		for (View *view : grid_members(*desktop)) {
			view->set_tiled(false);
		}
		server_.hint.set(std::format("Tiling off for Desktop {}",
			desktop->number()));
	}
}

void Canvas::reflow(DesktopId id) {
	Desktop *desktop = desktops.find(id);
	if (desktop == nullptr || !desktop->tiling()) {
		return;
	}
	std::vector<View *> members = grid_members(*desktop);
	if (members.empty()) {
		return;
	}
	flow_grid(members, tiling_area(*desktop));
	for (View *view : members) {
		view->grid = id; // held until it leaves the grid, whatever a zoom does
	}
}

void Canvas::settle(View &view) {
	// Opened, moved, minimized, maximized or restored: where its centre now
	// stands decides its grid again.
	view.grid = std::nullopt;
	bool gridded = false;
	for (const Desktop &desktop : desktops.all()) {
		if (desktop.tiling() && grid_member(view, desktop)) {
			gridded = true;
			break;
		}
	}
	if (!gridded) {
		view.set_tiled(false);
	}
	// Every tiled desktop re-flows: the one joined gained a window, the one
	// left lost it, and a grid nothing moved into lands where it stood.
	for (const Desktop &desktop : desktops.all()) {
		reflow(desktop.id());
	}
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
	std::vector<View *> incoming = views_in(from_area);
	for (View *view : incoming) {
		view->move(view->x + dx, view->y + dy);
		view->saved.x += dx;
		view->saved.y += dy;
	}
	int from_number = from_desktop->number(), into_number = into_desktop->number();
	bool from_tiled = from_desktop->tiling();
	desktops.remove(from);
	if (into_desktop->tiling()) {
		// The incoming windows join the grid.
		reflow(into);
	} else if (from_tiled) {
		// A tiled desktop combined into an untiled one leaves its windows
		// where the translation put them, no longer tiled.
		for (View *view : incoming) {
			view->set_tiled(false);
		}
	}
	server_.hint.set(std::format("Combined Desktop {} into Desktop {}",
		from_number, into_number));
}

wlr_box Canvas::spot_at(double cx, double cy, const Output &output) {
	if (Desktop *desktop = desktops.at(cx, cy)) {
		return desktop->frame();
	}
	return centred_box(output, cx, cy);
}

void Canvas::settle_scale(DesktopId id, double scale) {
	Desktop *desktop = desktops.find(id);
	if (desktop == nullptr) {
		return;
	}
	desktops.set_scale(id, scale);
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		if (output->overview.active() && output->overview.target().desktop == id) {
			output->overview.set_target(desktop->frame(), id, false);
		}
	}
	// A settled scale is a completed action, named. It re-lays out nothing:
	// the windows keep their size and layout, a tiled grid its boxes.
	server_.hint.set(std::format("Desktop {} · {} %", desktop->number(),
		static_cast<int>(std::round(scale * 100))));
}
