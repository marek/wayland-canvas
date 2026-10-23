#include "platform/output.h"

#include <cmath>
#include <cstdint>
#include <format>

#include "core/box.h"
#include "platform/layer.h"
#include "core/server.h"
#include "core/process.h"
#include "windows/view.h"

Output::Output(Server &server, wlr_output *wlr, uint32_t bit)
	: server(server), wlr(wlr), bit(bit), camera(*this), overview(*this) {
	wlr->data = this;
	frame_.connect<&Output::handle_frame>(wlr->events.frame, this);
	needs_frame_.connect<&Output::handle_needs_frame>(wlr->events.needs_frame, this);
	request_state_.connect<&Output::handle_request_state>(wlr->events.request_state, this);
	destroy_.connect<&Output::handle_destroy>(wlr->events.destroy, this);
}

Output::~Output() = default;

double Output::sharper_scale(double scale, const wlr_output &output) {
	return std::fmax(scale, output.scale);
}

void Output::tell_scale(wlr_surface *surface, double scale) {
	wlr_fractional_scale_v1_notify_scale(surface, scale);
	wlr_surface_set_preferred_buffer_scale(surface, static_cast<int32_t>(std::ceil(scale)));
}

void Output::damage() {
	dirty = true;
	wlr_output_schedule_frame(wlr);
}

void Output::visibility_changed() {
	damage();
	server.outputs.visibility_changed();
}

Rect Output::viewport() const {
	return camera.drawn().viewport(width, height);
}

Point Output::local_to_canvas(double ox, double oy) const {
	Camera cam = camera.drawn();
	return Point{
		.x = cam.cx + (ox - width / 2.0) / cam.zoom,
		.y = cam.cy + (oy - height / 2.0) / cam.zoom,
	};
}

Point Output::canvas_to_local(double cx, double cy) const {
	Camera cam = camera.drawn();
	return Point{
		.x = (cx - cam.cx) * cam.zoom + width / 2.0,
		.y = (cy - cam.cy) * cam.zoom + height / 2.0,
	};
}

wlr_box Output::usable_canvas_box(wlr_box area, double zoom) const {
	// The bars' output pixels in canvas units at that zoom.
	int left = canvas_units_covered(usable.x, zoom);
	int top = canvas_units_covered(usable.y, zoom);
	int right = canvas_units_covered(width - (usable.x + usable.width), zoom);
	int bottom = canvas_units_covered(height - (usable.y + usable.height), zoom);
	area.x += left;
	area.y += top;
	area.width -= left + right;
	area.height -= top + bottom;
	return area;
}

bool Output::tick(double now) {
	bool moving = camera.tick(now);

	double dt = now - fade_last_;
	if (!(dt >= 0)) {
		dt = 0;
	}
	if (dt > 50) {
		dt = 50;
	}
	fade_last_ = now;

	double alpha_target = is_panning() ? 1.0 : 0.0;
	double alpha = frames_alpha;
	if (alpha < alpha_target) {
		alpha = std::fmin(alpha_target, alpha + dt / 150.0);
	} else if (alpha > alpha_target) {
		alpha = std::fmax(alpha_target, alpha - dt / 150.0);
	}
	bool alpha_changed = alpha != frames_alpha;
	frames_alpha = alpha;
	if (alpha_changed) {
		damage();
	}

	return moving || alpha != alpha_target;
}

bool Output::is_panning() const {
	if (overview.active()) {
		return false;
	}
	if (camera.panning()) {
		return true;
	}
	if (server.input.grab.panning(*this)) {
		return true;
	}
	return false;
}

bool Output::interactive() const {
	if (overview.active()) {
		return false;
	}
	if (camera.current().zoom != resting_zoom()) {
		return false;
	}
	if (camera.zooming()) {
		return false;
	}
	if (server.gestures.zooming(*this)) {
		return false;
	}
	return true;
}

Desktop *Output::desktop_showing() const {
	Camera c = overview.active() ? overview.home() : camera.current();
	Rect viewport = c.viewport(width, height);
	return server.canvas.desktops.best_overlap(viewport);
}

double Output::resting_zoom() const {
	// The camera rests at the zoom of what it shows: the scale of the
	// desktop whose frame fills the screen; on a free view the zoom the
	// camera is coming to rest at, since only a zoom action changes it and
	// a pan, a scroll or a landing never does.
	if (overview.active()) {
		return overview.home().zoom;
	}
	Camera c = camera.current();
	Rect vp = c.viewport(width, height);
	for (const Desktop &d : server.canvas.desktops.all()) {
		Rect frame = d.rect();
		if (std::fabs(vp.x - frame.x) <= 1.0 && std::fabs(vp.y - frame.y) <= 1.0
				&& std::fabs(vp.w - frame.w) <= 1.0
				&& std::fabs(vp.h - frame.h) <= 1.0) {
			return d.scale();
		}
	}
	return camera.destination().zoom;
}

void Output::look_to(const Camera &to) {
	Camera at = camera.destination();
	if (!look_away_.has_value() || look_away_->left != at) {
		look_away_ = LookAway{.from = at, .left = at};
	}
	camera.jump(to);
	look_away_->left = camera.destination();
}

void Output::pan(double dx, double dy) {
	Camera next = camera.current();
	next.cx += dx;
	next.cy += dy;
	look_to(next);
}

bool Output::pan_back() {
	std::optional<LookAway> away = std::exchange(look_away_, std::nullopt);
	if (!away.has_value() || away->left != camera.destination()) {
		return false;
	}
	camera.animate_to(away->from);
	return true;
}

void Output::forget_look_away() {
	look_away_.reset();
}

void Output::goto_desktop(const Desktop &desktop) {
	if (overview.active()) {
		overview.set_target(desktop.frame(), desktop.id(), false);
		overview.exit();
		return;
	}
	// Going to a desktop eases onto it, at its scale, and gives the keyboard
	// to the window last used there.
	camera.animate_to(camera_for_box(desktop.frame(), desktop.scale()));
	server.focus.focus_in(desktop.frame());
}

void Output::goto_direction(double dx, double dy) {
	if (overview.active()) {
		return;
	}
	// From where the camera is going: a hop made while the last one still
	// eases walks on from its landing, desktop by desktop.
	Camera from = camera.destination();
	Desktop *leaving = server.canvas.desktops.best_overlap(from.viewport(width, height));
	Desktop *found = server.canvas.desktops.in_direction(from.cx, from.cy, dx, dy,
		id_of(leaving));
	if (found == nullptr) {
		// The normal view never hops into empty canvas, so a swipe cannot
		// lose the user's place: free areas are the overview's to target,
		// or a look away's with Super held.
		server.hint.set("No desktop that way");
		return;
	}
	goto_desktop(*found);
}

void Output::pin_desktop() {
	if (overview.active()) {
		overview.pin_target();
		return;
	}
	Rect vp = viewport();
	Desktop *desktop = server.canvas.desktops.best_overlap(vp);
	if (desktop != nullptr
			&& desktop->rect().overlap_area(vp) >= 0.9 * vp.w * vp.h) {
		server.hint.set("Already a desktop");
		return;
	}
	// Pinned at its zoom: the new desktop fills the screen at that scale.
	Desktop &created = server.canvas.desktops.create(box_rounded(vp), camera.current().zoom);
	server.hint.set(std::format("New Desktop {}", created.number()));
}

void Output::handle_frame() {
	bool animating = tick(now_ms());
	server.flush_pending();
	// needs_frame: screencopy clients and software cursors wait for a commit.
	if (!dirty && !animating && !wlr->needs_frame) {
		return;
	}
	dirty = false;
	server.painter.render(*this);
	if (animating) {
		wlr_output_schedule_frame(wlr);
	}
}

void Output::handle_needs_frame() {
	wlr_output_schedule_frame(wlr);
}

void Output::handle_request_state(wlr_output_event_request_state *event) {
	// Nested backends: the host resized our window.
	wlr_output_commit_state(wlr, event->state);
	damage();
	server.changed(Change::outputs); // a committed scale or mode changes the state
}

void Output::handle_destroy() {
	Server &server = this->server;
	server.output_gone(*this);
	server.outputs.erase(*this);
	server.outputs_changed();
}
