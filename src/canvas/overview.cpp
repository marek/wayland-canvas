#include "canvas/overview.h"

#include <cmath>
#include <format>
#include <optional>
#include <vector>

#include "canvas/camera.h"
#include "canvas/desktop.h"
#include "core/box.h"
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
#include "windows/view.h"

Overview::Overview(Output &output)
	: output_(output) {
}

bool Overview::active() const {
	return active_;
}

const OverviewTarget &Overview::target() const {
	return target_;
}

const Camera &Overview::home() const {
	return home_;
}

double Overview::zoom() const {
	return zoom_;
}

Camera Overview::camera_for(const Camera &home) const {
	Server &server = output_.server;
	double vw = output_.width, vh = output_.height;
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
	// Always further out than the view it leaves, a zoomed-out one included.
	double max_zoom = config::overview_max_zoom * std::fmin(home.zoom, 1.0);
	double zoom_c = fit_zoom(sym, vw, vh, config::overview_margin, max_zoom);
	double zoom_f = fit_zoom(content, vw, vh, config::overview_margin, max_zoom);
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

void Overview::reset_target() {
	if (Desktop *desktop = output_.desktop_showing()) {
		target_ = OverviewTarget{.box = desktop->frame(), .desktop = desktop->id()};
		return;
	}
	target_ = OverviewTarget{
		.box = box_rounded(home_.viewport(output_.width, output_.height)),
		.desktop = std::nullopt,
	};
}

Camera Overview::prepare() {
	output_.camera.stop();
	Camera home = output_.camera.current();
	// Home is where the camera came from: resting at the shown desktop's
	// scale, or 1 in a free view.
	home.zoom = output_.resting_zoom();
	home_ = home;
	reset_target();
	Camera far = camera_for(home);
	zoom_ = far.zoom;
	return far;
}

void Overview::enter() {
	if (active_) {
		return;
	}
	Camera far = prepare();
	finish_enter(far, 0);
}

void Overview::finish_enter(const Camera &far, double t) {
	output_.server.input.grab.cancel();
	// The overview is a move of its own: what it brings back is the view
	// a look away returns to (docs/intents/canvas/infinite-canvas.md).
	output_.forget_look_away();
	active_ = true;
	changed_ = true;
	output_.server.changed(Change::overview);
	wlr_seat_pointer_clear_focus(output_.server.seat);
	output_.camera.animate_zoom(home_, far, t, 1);
	output_.damage();
}

namespace {

// The zoom at which `box` fills the output's width: a free spot's own zoom.
double zoom_filling(const Output &output, const wlr_box &box) {
	return box.width > 0 ? static_cast<double>(output.width) / box.width : 1.0;
}

} // namespace

Camera Overview::target_camera() const {
	// A desktop at its scale; a free spot at the zoom that fills the screen
	// with its outline: the view the overview came from, or a screen-sized
	// spot at 1.
	Desktop *desktop = output_.server.canvas.desktops.find(target_.desktop);
	return camera_for_box(target_.box,
		desktop != nullptr ? desktop->scale() : zoom_filling(output_, target_.box));
}

void Overview::exit() {
	finish_exit(target_camera(), output_.camera.current(), 1);
}

void Overview::finish_exit(const Camera &near, const Camera &far, double t) {
	Server &server = output_.server;
	server.input.grab.cancel();
	active_ = false;
	changed_ = true;
	server.changed(Change::overview);
	output_.camera.animate_zoom(near, far, t, 0);
	server.focus_in(target_.box);
	server.pointer_focus_stale = true;
}

void Overview::cancel() {
	if (!active_) {
		return;
	}
	reset_target();
	finish_exit(home_, output_.camera.current(), 1);
}

void Overview::hop(double dx, double dy) {
	Server &server = output_.server;
	Point origin = box_centre(target_.box);
	Desktop *found = server.canvas.desktops.in_direction(origin.x, origin.y, dx, dy,
		target_.desktop);
	if (found != nullptr) {
		set_target(found->frame(), found->id(), true);
		return;
	}
	Compass q = nav_quantize_direction(dx, dy);
	double cx = origin.x + q.x * static_cast<double>(output_.width);
	double cy = origin.y + q.y * static_cast<double>(output_.height);
	Desktop *spot = server.canvas.desktops.at(cx, cy);
	set_target(server.canvas.spot_at(cx, cy, output_), id_of(spot), true);
}

void Overview::set_target(wlr_box box, std::optional<DesktopId> desktop,
		bool center) {
	target_ = OverviewTarget{.box = box, .desktop = desktop};
	output_.damage();
	if (center) {
		Camera next = output_.camera.current();
		Point centre = box_centre(box);
		next.cx = centre.x;
		next.cy = centre.y;
		output_.camera.animate_to(next);
	}
}

void Overview::target_point(double cx, double cy) {
	Desktop *desktop = output_.server.canvas.desktops.at(cx, cy);
	set_target(output_.server.canvas.spot_at(cx, cy, output_), id_of(desktop),
		false);
}

void Overview::zoom_by(double factor, double ox, double oy) {
	double zoom = clampd(output_.camera.current().zoom * factor,
		config::overview_min_zoom, config::overview_zoom_limit);
	Point c = output_.local_to_canvas(ox, oy);
	Camera next = {
		.cx = c.x - (ox - output_.width / 2.0) / zoom,
		.cy = c.y - (oy - output_.height / 2.0) / zoom,
		.zoom = zoom,
	};
	output_.camera.jump(next);
	zoom_ = zoom;
}

void Overview::keep_zoom() {
	zoom_ = output_.camera.current().zoom;
}

void Overview::tile(std::optional<DesktopId> desktop) {
	Server &server = output_.server;
	if (Desktop *found = server.canvas.desktops.find(desktop)) {
		server.canvas.toggle_tiling(found->id());
		return;
	}
	// A free spot holds no switch: its windows are tiled once, where they stand.
	server.canvas.tile(target_.box, output_);
}

void Overview::new_desktop(double cx, double cy) {
	wlr_box box = centred_box(output_.width, output_.height, cx, cy);
	Desktop &desktop = output_.server.canvas.desktops.create(box);
	set_target(box, desktop.id(), false);
	output_.server.hint.set(std::format("New Desktop {}", desktop.number()));
}

void Overview::pin_target() {
	Server &server = output_.server;
	if (server.canvas.desktops.find(target_.desktop) != nullptr) {
		server.hint.set("Already a desktop");
		return;
	}
	Desktop &desktop = server.canvas.desktops.create(target_.box,
		zoom_filling(output_, target_.box));
	target_.desktop = desktop.id();
	server.hint.set(std::format("New Desktop {}", desktop.number()));
}
