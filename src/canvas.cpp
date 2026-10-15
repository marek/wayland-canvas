#include "output.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
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

wlr_box rect_to_box(Rect r) {
	return wlr_box{
		static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y)),
		static_cast<int>(std::round(r.w)),
		static_cast<int>(std::round(r.h)),
	};
}

// A desktop's name, or nullopt for no desktop.
std::optional<DesktopId> id_of(const Desktop *desktop) {
	return desktop != nullptr ? std::optional<DesktopId>{desktop->id()} : std::nullopt;
}

} // namespace

// ---- canvas ----

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

// ---- overview ----

Camera Output::overview_camera(const Camera &home) const {
	double vw = width, vh = height;
	Rect content = home.viewport(vw, vh);
	for (const Desktop &desktop : server.canvas.desktops.all()) {
		content = content.united(desktop.rect());
	}
	// Every window on the canvas, so the overview shows what's out there.
	for (const View *view : server.views) {
		if (view->mapped && !view->minimized) {
			content = content.united(view->frame_rect());
		}
	}
	// Symmetric content around the home centre keeps the home screen steady
	// when it sits off-centre inside the content.
	double ex = std::fmax(home.cx - content.x, content.x + content.w - home.cx);
	double ey = std::fmax(home.cy - content.y, content.y + content.h - home.cy);
	Rect sym = {home.cx - ex, home.cy - ey, 2 * ex, 2 * ey};
	double zoom_c = fit_zoom(sym, vw, vh, config::overview_margin, config::overview_max_zoom);
	double zoom_f = fit_zoom(content, vw, vh, config::overview_margin, config::overview_max_zoom);
	if (zoom_c >= 0.75 * zoom_f) {
		return Camera{
			.cx = home.cx,
			.cy = home.cy,
			.zoom = std::fmax(zoom_c, config::overview_min_zoom),
		};
	}
	return Camera{
		.cx = content.cx(),
		.cy = content.cy(),
		.zoom = std::fmax(zoom_f, config::overview_min_zoom),
	};
}

Camera Output::overview_prepare() {
	camera.halt();
	Camera home = camera.current();
	home.zoom = 1;
	this->home = home;
	if (Desktop *desktop = desktop_showing()) {
		target = desktop->frame();
		target_desktop = desktop->id();
	} else {
		target = rect_to_box(home.viewport(width, height));
		target_desktop = std::nullopt;
	}
	Camera far = overview_camera(home);
	overview_zoom = far.zoom;
	return far;
}

void Output::overview_enter() {
	if (mode == Mode::overview) {
		return;
	}
	Camera far = overview_prepare();
	overview_finish_enter(far, 0);
}

void Output::overview_finish_enter(const Camera &far, double t) {
	server.input.cancel_grab();
	mode = Mode::overview;
	wlr_seat_pointer_clear_focus(server.seat);
	camera.animate_zoom(home, far, t, 1);
	damage();
}

void Output::overview_exit() {
	Camera near = camera_for_box(target);
	overview_finish_exit(near, camera.current(), 1);
}

void Output::overview_finish_exit(const Camera &near, const Camera &far, double t) {
	server.input.cancel_grab();
	mode = Mode::normal;
	camera.animate_zoom(near, far, t, 0);
	// Keyboard focus follows the target: if the focused window is not in it,
	// focus the topmost one that is.
	View *focused = server.focused_view;
	bool inside = false;
	if (focused != nullptr) {
		Point centre = box_centre(focused->frame_box());
		inside = wlr_box_contains_point(&target, centre.x, centre.y);
	}
	if (!inside) {
		std::vector<View *> in_target = server.canvas.views_in(target);
		if (!in_target.empty()) {
			server.focus_view(in_target.front());
		}
	}
	server.pointer_focus_stale = true;
}

void Output::overview_cancel() {
	if (mode != Mode::overview) {
		return;
	}
	if (Desktop *desktop = desktop_showing()) {
		target = desktop->frame();
		target_desktop = desktop->id();
	} else {
		target = rect_to_box(home.viewport(width, height));
		target_desktop = std::nullopt;
	}
	overview_finish_exit(home, camera.current(), 1);
}

void Output::overview_hop(double dx, double dy) {
	Point origin = box_centre(target);
	Desktop *found = server.canvas.desktops.in_direction(origin.x, origin.y, dx, dy,
		target_desktop);
	if (found != nullptr) {
		overview_set_target(found->frame(), found->id(), true);
		return;
	}
	Compass q = nav_quantize_direction(dx, dy);
	double cx = origin.x + q.x * static_cast<double>(width);
	double cy = origin.y + q.y * static_cast<double>(height);
	Desktop *spot = server.canvas.desktops.at(cx, cy);
	overview_set_target(server.canvas.spot_at(cx, cy, *this), id_of(spot), true);
}

void Output::overview_set_target(wlr_box box, std::optional<DesktopId> desktop,
		bool center) {
	target = box;
	target_desktop = desktop;
	damage();
	if (center) {
		Camera next = camera.current();
		Point centre = box_centre(box);
		next.cx = centre.x;
		next.cy = centre.y;
		camera.animate_to(next);
	}
}

void Output::overview_target_point(double cx, double cy) {
	Desktop *desktop = server.canvas.desktops.at(cx, cy);
	overview_set_target(server.canvas.spot_at(cx, cy, *this), id_of(desktop), false);
}

void Output::overview_zoom_by(double factor, double ox, double oy) {
	double zoom = clampd(camera.current().zoom * factor,
		config::overview_min_zoom, 0.75);
	Point c = local_to_canvas(ox, oy);
	Camera next = {
		.cx = c.x - (ox - width / 2.0) / zoom,
		.cy = c.y - (oy - height / 2.0) / zoom,
		.zoom = zoom,
	};
	camera.jump(next);
	overview_zoom = zoom;
}

void Output::overview_abandon(std::optional<DesktopId> desktop) {
	Desktop *found = server.canvas.desktops.find(desktop);
	if (found == nullptr) {
		server.hint.set("Nothing to abandon here");
		return;
	}
	int number = found->number();
	// The target box stays put and becomes a free spot.
	server.canvas.desktops.remove(found->id());
	server.hint.set(
		std::format("Abandoned Desktop {} — its windows stay on the canvas", number));
}

void Output::overview_tile(std::optional<DesktopId> desktop) {
	Desktop *found = server.canvas.desktops.find(desktop);
	server.canvas.tile(found != nullptr ? found->frame() : target, *this);
}

void Output::overview_combine(std::optional<DesktopId> into) {
	Desktop *into_desktop = server.canvas.desktops.find(into);
	if (into_desktop == nullptr) {
		server.hint.set("Nothing to combine here");
		return;
	}
	Desktop *from = server.canvas.desktops.nearest(into_desktop->id());
	if (from == nullptr) {
		server.hint.set("No other desktop to combine");
		return;
	}
	server.canvas.combine(into_desktop->id(), from->id(), from->frame());
}

void Output::overview_new_desktop(double cx, double cy) {
	wlr_box box = centred_box(*this, cx, cy);
	Desktop &desktop = server.canvas.desktops.create(box);
	overview_set_target(box, desktop.id(), false);
	server.hint.set(std::format("New Desktop {}", desktop.number()));
}
