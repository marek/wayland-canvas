#include "output.h"

#include <cmath>
#include <cstdint>
#include <format>

#include "layer.h"
#include "server.h"
#include "view.h"

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
	}
	output.damage();
}

void view_update_outputs(View &view, uint32_t mask) {
	uint32_t old = view.output_mask;
	view.output_mask = mask;
	// Like wlr_scene: a view scrolled off every output keeps its last
	// outputs, so panning away and back never makes clients re-render.
	if (mask == 0 || mask == old) {
		return;
	}
	double scale = 1;
	for (const std::unique_ptr<Output> &output : view.server.outputs) {
		if ((mask & output->bit) && output->wlr->scale > scale) {
			scale = output->wlr->scale;
		}
	}
	view.for_each_surface([&](wlr_surface *surface, int, int) {
		for (const std::unique_ptr<Output> &output : view.server.outputs) {
			if (mask & output->bit) {
				wlr_surface_send_enter(surface, output->wlr);
			} else {
				wlr_surface_send_leave(surface, output->wlr);
			}
		}
		wlr_fractional_scale_v1_notify_scale(surface, scale);
		wlr_surface_set_preferred_buffer_scale(surface, (int32_t)std::ceil(scale));
	});
	// The foreign handle, when the view has one, follows the same outputs.
	for (const std::unique_ptr<Output> &output : view.server.outputs) {
		bool was = old & output->bit, is = mask & output->bit;
		if (is && !was) {
			view.foreign_output_enter(*output);
		} else if (was && !is) {
			view.foreign_output_leave(*output);
		}
	}
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

Output::Output(Server &server, wlr_output *wlr)
	: server(server), wlr(wlr), camera(*this), overview(*this) {
	uint32_t used = 0;
	for (const std::unique_ptr<Output> &existing : server.outputs) {
		used |= existing->bit;
	}
	bit = 1;
	while (used & bit) {
		bit <<= 1;
	}
	wlr->data = this;
	frame_.connect<&Output::handle_frame>(wlr->events.frame, this);
	needs_frame_.connect<&Output::handle_needs_frame>(wlr->events.needs_frame, this);
	request_state_.connect<&Output::handle_request_state>(wlr->events.request_state, this);
	destroy_.connect<&Output::handle_destroy>(wlr->events.destroy, this);
}

Output::~Output() = default;

void Output::damage() {
	dirty = true;
	wlr_output_schedule_frame(wlr);
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

wlr_box Output::usable_canvas_box(wlr_box area) const {
	int left = usable.x, top = usable.y;
	int right = width - (usable.x + usable.width);
	int bottom = height - (usable.y + usable.height);
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
	if (server.gestures.panning(*this)) {
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
	if (camera.current().zoom != 1) {
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

void Output::pan(double dx, double dy) {
	Camera next = camera.current();
	next.cx += dx;
	next.cy += dy;
	camera.jump(next);
}

void Output::goto_box(wlr_box box) {
	if (overview.active()) {
		Point centre = box_centre(box);
		overview.set_target(box,
			id_of(server.canvas.desktops.at(centre.x, centre.y)), false);
		overview.exit();
		return;
	}
	camera.animate_to(camera_for_box(box));
}

void Output::goto_direction(double dx, double dy) {
	if (overview.active()) {
		return;
	}
	Desktop *found = server.canvas.desktops.in_direction(camera.current().cx,
		camera.current().cy, dx, dy, id_of(desktop_showing()));
	if (found != nullptr) {
		camera.animate_to(camera_for_box(found->frame()));
		return;
	}
	Compass q = nav_quantize_direction(dx, dy);
	Camera to = camera.current();
	to.cx += q.x * static_cast<double>(width);
	to.cy += q.y * static_cast<double>(height);
	to.zoom = 1;
	camera.animate_to(to);
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
	Desktop &created = server.canvas.desktops.create(rect_to_box(vp));
	server.hint.set(std::format("New Desktop {}", created.number()));
}

void Server::reveal_view(View &view) {
	Output *output = output_under_cursor();
	if (output == nullptr) {
		return;
	}
	wlr_box frame = view.frame_box();
	Point centre = box_centre(frame);
	if (!output->overview.active()) {
		Rect vp = output->viewport();
		if (frame.x >= vp.x && frame.y >= vp.y
				&& frame.x + frame.width <= vp.x + vp.w
				&& frame.y + frame.height <= vp.y + vp.h) {
			return;
		}
	}
	Desktop *desktop = canvas.desktop_of(view);
	wlr_box box = desktop != nullptr ? desktop->frame()
		: centred_box(*output, centre.x, centre.y);
	if (output->overview.active()) {
		output->overview.set_target(box, id_of(desktop), false);
		output->overview.exit();
		return;
	}
	output->camera.animate_to(camera_for_box(box));
}

void Server::damage_all() {
	for (const std::unique_ptr<Output> &output : outputs) {
		output->damage();
	}
}

Output *Server::output_at(double lx, double ly) {
	wlr_output *wlr = wlr_output_layout_output_at(output_layout, lx, ly);
	return wlr != nullptr ? static_cast<Output *>(wlr->data) : nullptr;
}

Output *Server::output_under_cursor() {
	Output *output = output_at(cursor->x, cursor->y);
	if (output == nullptr && !outputs.empty()) {
		output = outputs.front().get();
	}
	return output;
}

void Server::update_visibility() {
	visibility_dirty = false;
	for (View *view : views) {
		uint32_t mask = 0;
		if (view->mapped && !view->minimized) {
			Rect frame = view->frame_rect();
			for (const std::unique_ptr<Output> &output : outputs) {
				if (frame.intersects(output->viewport())) {
					mask |= output->bit;
				}
			}
		}
		view_update_outputs(*view, mask);
	}
}

void Output::handle_frame() {
	bool animating = tick(now_ms());
	Server &server = this->server;
	if (server.visibility_dirty) {
		server.update_visibility();
	}
	if (server.pointer_focus_stale) {
		server.pointer_focus_stale = false;
		server.input.rebase_cursor();
	}
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
}

void Output::handle_destroy() {
	Server &server = this->server;

	server.input.grab.forget(*this);
	server.gestures.output_gone(*this);
	for (View *view : server.views) {
		view->output_mask &= ~bit;
	}
	server.launcher.output_destroyed(*this);

	// The layer surfaces die with the output; destroying each wlroots object
	// runs its own destroy handler, which takes it out of these vectors.
	std::vector<LayerSurface *> doomed;
	for (const std::vector<std::unique_ptr<LayerSurface>> &vec : layers) {
		for (const std::unique_ptr<LayerSurface> &layer : vec) {
			doomed.push_back(layer.get());
		}
	}
	for (LayerSurface *layer : doomed) {
		layer->attached_ = false;
		wlr_layer_surface_v1_destroy(layer->wlr);
	}

	server.visibility_dirty = true;
	destroy_owned(server.outputs, this);
}

void Server::handle_new_output(wlr_output *wlr) {
	if (wlr->non_desktop) {
		return;
	}
	uint32_t used = 0;
	for (const std::unique_ptr<Output> &existing : outputs) {
		used |= existing->bit;
	}
	if (used == UINT32_MAX) {
		wlr_log(WLR_ERROR, "too many outputs, ignoring %s", wlr->name);
		return;
	}
	wlr_output_init_render(wlr, allocator, renderer);

	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
	if (mode != nullptr) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_state_set_scale(&state, pick_scale(*this, wlr, mode));
	if (!wlr_output_commit_state(wlr, &state)) {
		wlr_log(WLR_ERROR, "failed to enable %s with its preferred mode", wlr->name);
		wlr_output_state_finish(&state);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_commit_state(wlr, &state);
	}
	wlr_output_state_finish(&state);

	outputs.push_back(std::make_unique<Output>(*this, wlr));
	Output &output = *outputs.back();

	wlr_output_layout_add_auto(output_layout, wlr);
	update_output_geometry(output);
	// The canvas starts out aligned with the output layout.
	output.camera.jump(Camera{
		.cx = output.lx + output.width / 2.0,
		.cy = output.ly + output.height / 2.0,
		.zoom = 1,
	});
	if (canvas.desktops.empty()) {
		canvas.desktops.create(
			wlr_box{output.lx, output.ly, output.width, output.height});
	}
	wlr_log(WLR_INFO, "output %s: %dx%d logical at scale %.2f", wlr->name,
		output.width, output.height, wlr->scale);
	visibility_dirty = true;
	output.damage();
}

void Server::handle_layout_change() {
	for (const std::unique_ptr<Output> &output : outputs) {
		update_output_geometry(*output);
	}
	visibility_dirty = true;
}

void Server::init_outputs() {
	new_output_.connect<&Server::handle_new_output>(backend->events.new_output, this);
	layout_change_.connect<&Server::handle_layout_change>(output_layout->events.change, this);
}

void Server::finish_outputs() {
	new_output_.disconnect();
	layout_change_.disconnect();
}
