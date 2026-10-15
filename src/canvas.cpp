#include "output.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <vector>

#include "config.h"
#include "desktop.h"
#include "server.h"
#include "view.h"

namespace {

// Cap on views collected for tiling / combining; the rest stay untouched.
constexpr std::size_t box_views_max = 64;

// Momentum slower than this (canvas px per ms) is indistinguishable from
// still and ends the coast.
constexpr double coast_stop_speed = 0.02;

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

} // namespace

// ---- camera ----

void Output::stop_camera() {
	anim.kind = AnimKind::none;
	coasting = false;
	// Restart the tick clock so the next tick's dt is a real interval.
	last_tick = now_ms();
}

void Output::set_camera(const Camera &to) {
	stop_camera();
	cam = to;
	server.visibility_dirty = true;
	server.pointer_focus_stale = true;
	damage();
}

void Output::animate_camera(const Camera &to) {
	stop_camera();
	anim.kind = AnimKind::lerp;
	anim.a = cam;
	anim.b = to;
	anim.start = now_ms();
	anim.duration = config::anim_ms;
	damage();
}

void Output::animate_zoom(const Camera &near, const Camera &far, double p0, double p1) {
	stop_camera();
	anim.kind = AnimKind::zoom;
	anim.a = near;
	anim.b = far;
	anim.p0 = p0;
	anim.p1 = p1;
	anim.start = now_ms();
	anim.duration = config::anim_ms * clampd(std::fabs(p1 - p0), 0.4, 1);
	damage();
}

bool Output::tick(double now) {
	double dt = now - last_tick;
	if (!(dt >= 0)) {
		dt = 0;
	}
	if (dt > 50) {
		dt = 50;
	}
	last_tick = now;

	Camera next = cam;
	bool busy = false;

	if (anim.kind != AnimKind::none) {
		double t = (now - anim.start) / anim.duration;
		if (t >= 1) {
			// Land exactly on the end camera, no float drift.
			if (anim.kind == AnimKind::lerp || anim.p1 >= 1) {
				next = anim.b;
			} else if (anim.p1 <= 0) {
				next = anim.a;
			} else {
				next = camera_zoom_path(anim.a, anim.b, anim.p1);
			}
			anim.kind = AnimKind::none;
		} else {
			double e = ease_out_cubic(t);
			if (anim.kind == AnimKind::lerp) {
				next = camera_lerp(anim.a, anim.b, e);
			} else {
				double p = anim.p0 + (anim.p1 - anim.p0) * e;
				next = camera_zoom_path(anim.a, anim.b, p);
			}
			busy = true;
		}
	}

	if (coasting) {
		next.cx += vel_x * dt;
		next.cy += vel_y * dt;
		double decay = std::exp(-dt / config::inertia_tau_ms);
		vel_x *= decay;
		vel_y *= decay;
		if (std::hypot(vel_x, vel_y) < coast_stop_speed) {
			coasting = false;
			vel_x = 0;
			vel_y = 0;
		} else {
			busy = true;
		}
	}

	double alpha_target = is_panning() ? 1.0 : 0.0;
	double alpha = frames_alpha;
	if (alpha < alpha_target) {
		alpha = std::fmin(alpha_target, alpha + dt / 150.0);
	} else if (alpha > alpha_target) {
		alpha = std::fmax(alpha_target, alpha - dt / 150.0);
	}

	bool cam_changed = next.cx != cam.cx || next.cy != cam.cy || next.zoom != cam.zoom;
	bool alpha_changed = alpha != frames_alpha;

	cam = next;
	frames_alpha = alpha;

	if (cam_changed) {
		dirty = true;
		server.visibility_dirty = true;
		server.pointer_focus_stale = true;
	}
	if (cam_changed || alpha_changed || busy) {
		damage();
	}

	return busy || alpha != alpha_target;
}

Camera camera_for_box(wlr_box box) {
	return Camera{
		.cx = box.x + box.width / 2.0,
		.cy = box.y + box.height / 2.0,
		.zoom = 1,
	};
}

bool Output::is_panning() const {
	if (mode != Mode::normal) {
		return false;
	}
	if (coasting || anim.kind == AnimKind::lerp) {
		return true;
	}
	if (server.gesture.kind == GestureKind::pan && server.gesture.output == this) {
		return true;
	}
	if (server.input.cursor_mode == CursorMode::pan && server.input.grab_output == this) {
		return true;
	}
	return false;
}

bool Output::interactive() const {
	if (mode != Mode::normal) {
		return false;
	}
	if (cam.zoom != 1) {
		return false;
	}
	if (anim.kind == AnimKind::zoom) {
		return false;
	}
	if (server.gesture.output == this) {
		switch (server.gesture.kind) {
		case GestureKind::zoom_out:
		case GestureKind::zoom_in:
		case GestureKind::zoom_further:
		case GestureKind::overview_zoom:
			return false;
		default:
			break;
		}
	}
	return true;
}

// ---- desktops ----

Desktop &Server::create_desktop(wlr_box box) {
	int id = 1;
	while (desktop_by_id(id) != nullptr) {
		id++;
	}
	std::unique_ptr<Desktop> owned = std::make_unique<Desktop>();
	Desktop &desktop = *owned;
	desktop.id = id;
	desktop.box = box;
	desktops.insert(desktops.begin(), std::move(owned));
	damage_all();
	return desktop;
}

void Server::destroy_desktop(Desktop &desktop) {
	for (const std::unique_ptr<Output> &output : outputs) {
		if (output->target_desktop == &desktop) {
			output->target_desktop = nullptr;
		}
	}
	if (input.grab_desktop == &desktop) {
		input.cancel_grab();
		input.grab_desktop = nullptr;
	}
	if (input.drop_desktop == &desktop) {
		input.drop_desktop = nullptr;
	}
	if (gesture.desktop == &desktop) {
		gesture.desktop = nullptr;
		// An abandon swipe has nothing left to slide; tile and combine fall back to the target.
		if (gesture.kind == GestureKind::abandon) {
			gesture.kind = GestureKind::ignored;
		}
	}
	if (gesture.merge_from == &desktop) {
		gesture.merge_from = nullptr;
	}
	destroy_owned(desktops, &desktop); // releases the label texture
	damage_all();
}

Desktop *Server::desktop_at(double cx, double cy) {
	for (const std::unique_ptr<Desktop> &desktop : desktops) {
		if (wlr_box_contains_point(&desktop->box, cx, cy)) {
			return desktop.get();
		}
	}
	return nullptr;
}

Desktop *Server::desktop_of(const View &view) {
	Point centre = box_centre(view.frame_box());
	return desktop_at(centre.x, centre.y);
}

Desktop *Server::desktop_by_id(int id) {
	for (const std::unique_ptr<Desktop> &desktop : desktops) {
		if (desktop->id == id) {
			return desktop.get();
		}
	}
	return nullptr;
}

Desktop *Server::desktop_best_overlap(const Rect &area) {
	Desktop *best = nullptr;
	double best_overlap = area.w * area.h * 0.5;
	for (const std::unique_ptr<Desktop> &desktop : desktops) {
		double overlap = box_to_rect(desktop->box).overlap_area(area);
		if (overlap < area.w * area.h * 0.5) {
			continue;
		}
		if (best == nullptr || overlap > best_overlap) {
			best = desktop.get();
			best_overlap = overlap;
		}
	}
	return best;
}

Desktop *Output::desktop_showing() const {
	Camera c = mode == Mode::overview ? home : cam;
	Rect viewport = c.viewport(width, height);
	return server.desktop_best_overlap(viewport);
}

Desktop *Server::desktop_nearest(const Desktop &to) {
	Point origin = box_centre(to.box);
	Desktop *best = nullptr;
	double best_dist = INFINITY;
	for (const std::unique_ptr<Desktop> &desktop : desktops) {
		if (desktop.get() == &to) {
			continue;
		}
		Point centre = box_centre(desktop->box);
		double dx = centre.x - origin.x, dy = centre.y - origin.y;
		double dist = dx * dx + dy * dy;
		if (dist < best_dist) {
			best_dist = dist;
			best = desktop.get();
		}
	}
	return best;
}

void Server::raise_desktop(Desktop &desktop) {
	auto it = std::ranges::find_if(desktops,
		[&desktop](const std::unique_ptr<Desktop> &p) { return p.get() == &desktop; });
	if (it == desktops.end()) {
		return;
	}
	std::unique_ptr<Desktop> owned = std::move(*it);
	desktops.erase(it);
	desktops.insert(desktops.begin(), std::move(owned));
}

// ---- window actions ----

std::vector<View *> Server::views_in_box(wlr_box box) {
	std::vector<View *> out;
	for (View *view : views) {
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

void Server::tile(wlr_box area, Output *output) {
	std::vector<View *> in_box = views_in_box(area);
	if (in_box.empty()) {
		hint.set("No windows to tile here");
		return;
	}
	Rect usable = box_to_rect(output->usable_canvas_box(area));
	std::size_t n = in_box.size();
	auto cols = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
	std::size_t rows = (n + cols - 1) / cols;
	double band = usable.h / static_cast<double>(rows);
	std::vector<Rect> frames;
	frames.reserve(n);
	for (View *view : in_box) {
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
		in_box[order[i]]->set_frame(cell, true);
	}
	hint.set(std::format("Tiled {} window{}", n, n == 1 ? "" : "s"));
}

void Server::combine(Desktop &into, Desktop &from, wlr_box from_box) {
	if (&into == &from) {
		return;
	}
	int dx = into.box.x - from_box.x;
	int dy = into.box.y - from_box.y;
	std::vector<View *> in_box = views_in_box(from_box);
	for (View *view : in_box) {
		view->move(view->x + dx, view->y + dy);
		view->saved.x += dx;
		view->saved.y += dy;
	}
	int from_id = from.id, into_id = into.id;
	destroy_desktop(from);
	hint.set(std::format("Combined Desktop {} into Desktop {}", from_id, into_id));
}

// ---- navigation ----

void Output::pan(double dx, double dy) {
	Camera next = cam;
	next.cx += dx;
	next.cy += dy;
	set_camera(next);
}

void Output::goto_box(wlr_box box) {
	if (mode == Mode::overview) {
		Point centre = box_centre(box);
		overview_set_target(box, server.desktop_at(centre.x, centre.y), false);
		overview_exit();
		return;
	}
	animate_camera(camera_for_box(box));
}

void Output::goto_direction(double dx, double dy) {
	if (mode != Mode::normal) {
		return;
	}
	Desktop *found = nullptr;
	std::vector<Rect> cands;
	cands.reserve(server.desktops.size());
	for (const std::unique_ptr<Desktop> &desktop : server.desktops) {
		cands.push_back(box_to_rect(desktop->box));
	}
	if (!cands.empty()) {
		Desktop *showing = desktop_showing();
		std::optional<std::size_t> exclude;
		for (std::size_t i = 0; i < server.desktops.size(); i++) {
			if (server.desktops[i].get() == showing) {
				exclude = i;
			}
		}
		std::optional<std::size_t> best = nav_pick_direction(cam.cx, cam.cy, dx, dy,
			cands, exclude, config::hop_cone_deg);
		if (best.has_value()) {
			found = server.desktops[*best].get();
		}
	}
	if (found != nullptr) {
		animate_camera(camera_for_box(found->box));
		return;
	}
	Compass q = nav_quantize_direction(dx, dy);
	Camera to = cam;
	to.cx += q.x * static_cast<double>(width);
	to.cy += q.y * static_cast<double>(height);
	to.zoom = 1;
	animate_camera(to);
}

void Server::reveal_view(View &view) {
	Output *output = output_under_cursor();
	if (output == nullptr) {
		return;
	}
	wlr_box frame = view.frame_box();
	Point centre = box_centre(frame);
	if (output->mode == Mode::normal) {
		Rect vp = output->viewport();
		if (frame.x >= vp.x && frame.y >= vp.y
				&& frame.x + frame.width <= vp.x + vp.w
				&& frame.y + frame.height <= vp.y + vp.h) {
			return;
		}
	}
	Desktop *desktop = desktop_of(view);
	wlr_box box = desktop != nullptr ? desktop->box : centred_box(*output, centre.x, centre.y);
	if (output->mode == Mode::overview) {
		output->overview_set_target(box, desktop, false);
		output->overview_exit();
		return;
	}
	output->animate_camera(camera_for_box(box));
}

void Output::pin_desktop() {
	if (mode == Mode::overview) {
		if (target_desktop != nullptr) {
			server.hint.set("Already a desktop");
			return;
		}
		Desktop &desktop = server.create_desktop(target);
		target_desktop = &desktop;
		server.hint.set(std::format("New Desktop {}", desktop.id));
		return;
	}
	Rect vp = viewport();
	Desktop *desktop = server.desktop_best_overlap(vp);
	if (desktop != nullptr
			&& box_to_rect(desktop->box).overlap_area(vp) >= 0.9 * vp.w * vp.h) {
		server.hint.set("Already a desktop");
		return;
	}
	desktop = &server.create_desktop(rect_to_box(vp));
	server.hint.set(std::format("New Desktop {}", desktop->id));
}

// ---- overview ----

Camera Output::overview_camera(const Camera &home) const {
	double vw = width, vh = height;
	Rect content = home.viewport(vw, vh);
	for (const std::unique_ptr<Desktop> &desktop : server.desktops) {
		content = content.united(box_to_rect(desktop->box));
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
	stop_camera();
	Camera home = cam;
	home.zoom = 1;
	this->home = home;
	Desktop *desktop = desktop_showing();
	if (desktop != nullptr) {
		target = desktop->box;
		target_desktop = desktop;
	} else {
		target = rect_to_box(home.viewport(width, height));
		target_desktop = nullptr;
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
	animate_zoom(this->home, far, t, 1);
	damage();
}

void Output::overview_exit() {
	Camera near = camera_for_box(target);
	overview_finish_exit(near, cam, 1);
}

void Output::overview_finish_exit(const Camera &near, const Camera &far, double t) {
	server.input.cancel_grab();
	mode = Mode::normal;
	animate_zoom(near, far, t, 0);
	// Keyboard focus follows the target: if the focused window is not in it,
	// focus the topmost one that is.
	View *focused = server.focused_view;
	bool inside = false;
	if (focused != nullptr) {
		Point centre = box_centre(focused->frame_box());
		inside = wlr_box_contains_point(&target, centre.x, centre.y);
	}
	if (!inside) {
		std::vector<View *> in_target = server.views_in_box(target);
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
	Desktop *desktop = desktop_showing();
	if (desktop != nullptr) {
		target = desktop->box;
		target_desktop = desktop;
	} else {
		target = rect_to_box(home.viewport(width, height));
		target_desktop = nullptr;
	}
	overview_finish_exit(home, cam, 1);
}

void Output::overview_hop(double dx, double dy) {
	Point origin = box_centre(target);
	Desktop *found = nullptr;
	std::vector<Rect> cands;
	cands.reserve(server.desktops.size());
	for (const std::unique_ptr<Desktop> &desktop : server.desktops) {
		cands.push_back(box_to_rect(desktop->box));
	}
	if (!cands.empty()) {
		std::optional<std::size_t> exclude;
		for (std::size_t i = 0; i < server.desktops.size(); i++) {
			if (server.desktops[i].get() == target_desktop) {
				exclude = i;
			}
		}
		std::optional<std::size_t> best = nav_pick_direction(origin.x, origin.y, dx, dy,
			cands, exclude, config::hop_cone_deg);
		if (best.has_value()) {
			found = server.desktops[*best].get();
		}
	}
	if (found != nullptr) {
		overview_set_target(found->box, found, true);
		return;
	}
	Compass q = nav_quantize_direction(dx, dy);
	double cx = origin.x + q.x * static_cast<double>(width);
	double cy = origin.y + q.y * static_cast<double>(height);
	overview_set_target(target_box_at(cx, cy), server.desktop_at(cx, cy), true);
}

void Output::overview_set_target(wlr_box box, Desktop *desktop, bool center) {
	target = box;
	target_desktop = desktop;
	damage();
	if (center) {
		Camera next = cam;
		Point centre = box_centre(box);
		next.cx = centre.x;
		next.cy = centre.y;
		animate_camera(next);
	}
}

void Output::overview_target_point(double cx, double cy) {
	overview_set_target(target_box_at(cx, cy), server.desktop_at(cx, cy), false);
}

void Output::overview_zoom_by(double factor, double ox, double oy) {
	double zoom = clampd(cam.zoom * factor, config::overview_min_zoom, 0.75);
	Point c = local_to_canvas(ox, oy);
	Camera next = {
		.cx = c.x - (ox - width / 2.0) / zoom,
		.cy = c.y - (oy - height / 2.0) / zoom,
		.zoom = zoom,
	};
	set_camera(next);
	overview_zoom = zoom;
}

void Output::overview_abandon(Desktop *desktop) {
	if (desktop == nullptr) {
		server.hint.set("Nothing to abandon here");
		return;
	}
	int id = desktop->id;
	// destroy_desktop clears target_desktop if it was this one; the target
	// box stays put and becomes a free spot.
	server.destroy_desktop(*desktop);
	server.hint.set(
		std::format("Abandoned Desktop {} — its windows stay on the canvas", id));
}

void Output::overview_tile(Desktop *desktop) {
	server.tile(desktop != nullptr ? desktop->box : target, this);
}

void Output::overview_combine(Desktop *into) {
	if (into == nullptr) {
		server.hint.set("Nothing to combine here");
		return;
	}
	Desktop *from = server.desktop_nearest(*into);
	if (from == nullptr) {
		server.hint.set("No other desktop to combine");
		return;
	}
	server.combine(*into, *from, from->box);
}

void Output::overview_new_desktop(double cx, double cy) {
	wlr_box box = centred_box(*this, cx, cy);
	Desktop &desktop = server.create_desktop(box);
	overview_set_target(box, &desktop, false);
	server.hint.set(std::format("New Desktop {}", desktop.id));
}

wlr_box Output::target_box_at(double cx, double cy) const {
	Desktop *desktop = server.desktop_at(cx, cy);
	if (desktop != nullptr) {
		return desktop->box;
	}
	return centred_box(*this, cx, cy);
}
