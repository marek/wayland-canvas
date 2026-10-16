#include <cmath>
#include "platform/layer.h"
#include "platform/output.h"
#include "core/server.h"

namespace {

// ---- geometry ----

Rect rect_of_box(const wlr_box &box) {
	return Rect{double(box.x), double(box.y), double(box.width), double(box.height)};
}

wlr_box box_of_viewport(const Rect &r) {
	return wlr_box{
		static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y)),
		static_cast<int>(std::ceil(r.w)),
		static_cast<int>(std::ceil(r.h)),
	};
}

// ---- maximize/fullscreen areas ----

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
	Output *picked = nullptr;
	for (const std::unique_ptr<Output> &output : view.server.outputs) {
		if (output->overview.active()) {
			continue;
		}
		if (output->viewport().contains(frame.cx(), frame.cy())) {
			picked = output.get();
			break;
		}
	}
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

// Fullscreen covers the whole output viewport, no decorations, no inset.
wlr_box fullscreen_area(View &view) {
	Rect frame = view.frame_rect();
	Output *picked = nullptr;
	for (const std::unique_ptr<Output> &output : view.server.outputs) {
		if (output->viewport().contains(frame.cx(), frame.cy())) {
			picked = output.get();
			break;
		}
	}
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

// ---- placement ----

void place(View &view) {
	Output *output = view.server.output_under_cursor();
	if (output == nullptr) {
		view.x = 0;
		view.y = 0;
		return;
	}
	wlr_box geo = view.wlr->base->geometry;
	int tb = view.deco.titlebar();
	int bw = view.deco.border();
	int fw = geo.width + 2 * bw;
	int fh = geo.height + tb + 2 * bw;

	if (view.fullscreen) {
		wlr_box area = fullscreen_area(view);
		view.x = area.x;
		view.y = area.y;
		return;
	}
	if (view.maximized) {
		wlr_box area = maximize_area(view);
		view.x = area.x;
		view.y = area.y + view.deco.titlebar();
		return;
	}

	// Inside the screen minus bars (exclusive zones), or the overview target
	// at the zoom leaving into it rests at.
	bool overview = output->overview.active();
	wlr_box screen = overview ? output->overview.target().box
		: box_of_viewport(output->viewport());
	double zoom = overview ? output->overview.target_camera().zoom
		: output->camera.drawn().zoom;
	Rect area = rect_of_box(output->usable_canvas_box(screen, zoom));
	double px = area.x + (area.w - fw) / 2.0;
	double py = area.y + (area.h - fh) / 2.0;
	if (fw > area.w) {
		px = area.x;
	}
	if (fh > area.h) {
		py = area.y;
	}
	px = clampd(px, area.x, std::fmax(area.x, area.x + area.w - fw));
	py = clampd(py, area.y, std::fmax(area.y, area.y + area.h - fh));
	// px/py place the frame; view.x/y is the content inside it.
	int x = static_cast<int>(std::round(px)) + bw;
	int y = static_cast<int>(std::round(py)) + tb + bw;

	// Cascade by 32px while another view occupies the chosen position.
	int step = 0;
	while (step < 64) {
		bool clash = false;
		for (View *other : view.server.views) {
			if (other == &view || !other->mapped) {
				continue;
			}
			if (other->x == x + 32 * step && other->y == y + 32 * step) {
				clash = true;
				break;
			}
		}
		if (!clash) {
			break;
		}
		step++;
	}
	x += 32 * step;
	y += 32 * step;
	// Keep the title bar on screen after cascading.
	int frame_top = y - tb - bw;
	int bottom = static_cast<int>(std::floor(area.y + area.h));
	if (frame_top + tb > bottom) {
		frame_top = bottom - tb;
	}
	if (frame_top < static_cast<int>(std::ceil(area.y))) {
		frame_top = static_cast<int>(std::ceil(area.y));
	}
	view.x = x;
	view.y = frame_top + tb + bw;
}

bool is_initialized(const View &view) {
	return view.wlr->base->initialized;
}

// ---- interactive move/resize requests ----

// Only honour requests stemming from a real button press on the focused
// window; anything else is a client trying its luck.
bool grab_allowed(const Server &server, const View &view) {
	return server.focused_view == &view
		&& !server.input.grab.active()
		&& server.seat->pointer_state.button_count > 0;
}

// ---- popups ----

// Owner view of a popup: walk the popup parent chain to its root surface.
View *popup_owner_view(wlr_xdg_popup *popup) {
	wlr_surface *surface = popup->parent;
	if (surface == nullptr) {
		return nullptr;
	}
	return View::from_surface(surface);
}

LayerSurface *popup_owner_layer(wlr_xdg_popup *popup) {
	wlr_surface *surface = popup->parent;
	if (surface == nullptr) {
		return nullptr;
	}
	while (true) {
		wlr_xdg_popup *parent = wlr_xdg_popup_try_from_wlr_surface(surface);
		if (parent == nullptr || parent->parent == nullptr) {
			break;
		}
		surface = parent->parent;
	}
	wlr_surface *root = wlr_surface_get_root_surface(surface);
	wlr_layer_surface_v1 *layer_surface =
		wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (layer_surface == nullptr) {
		return nullptr;
	}
	return static_cast<LayerSurface *>(layer_surface->data);
}

// Constrain the popup to the area its owner is visible in; the box must be
// in the root parent surface's coordinate space. Returns false when no
// owner was found (the caller then replies with a bare configure).
bool popup_unconstrain(Popup &popup) {
	wlr_xdg_popup *xdg_popup = popup.wlr;

	View *view = popup_owner_view(xdg_popup);
	if (view != nullptr) {
		Server &server = view->server;
		// The output whose viewport shows the frame centre, else the one
		// under the cursor.
		Rect frame = view->frame_rect();
		Output *picked = nullptr;
		for (const std::unique_ptr<Output> &output : server.outputs) {
			if (output->viewport().contains(frame.cx(), frame.cy())) {
				picked = output.get();
				break;
			}
		}
		if (picked == nullptr) {
			picked = server.output_under_cursor();
		}
		if (picked != nullptr) {
			Rect viewport = picked->viewport();
			wlr_box geo = view->wlr->base->geometry;
			// Canvas -> toplevel surface-local: the surface origin sits at
			// (view->x - geo.x, view->y - geo.y) on the canvas.
			wlr_box box{
				static_cast<int>(std::floor(viewport.x - (view->x - geo.x))),
				static_cast<int>(std::floor(viewport.y - (view->y - geo.y))),
				static_cast<int>(std::ceil(viewport.w)),
				static_cast<int>(std::ceil(viewport.h)),
			};
			wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
			return true;
		}
	}

	LayerSurface *layer = popup_owner_layer(xdg_popup);
	if (layer != nullptr) {
		// Output-local rect shifted into the layer surface's space.
		wlr_box box{
			-layer->geo.x,
			-layer->geo.y,
			layer->output.width,
			layer->output.height,
		};
		wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
		return true;
	}
	return false;
}

} // namespace

// ---- geometry ----

wlr_box View::content_box() const {
	wlr_box geo = wlr->base->geometry;
	return wlr_box{x, y, geo.width, geo.height};
}

wlr_box View::frame_box() const {
	wlr_box content = content_box();
	int tb = deco.titlebar();
	int bw = deco.border();
	return wlr_box{
		content.x - bw,
		content.y - tb - bw,
		content.width + 2 * bw,
		content.height + tb + 2 * bw,
	};
}

Rect View::frame_rect() const {
	return rect_of_box(frame_box());
}

// ---- state helpers ----

void View::damage() {
	Rect frame = frame_rect();
	for (const std::unique_ptr<Output> &output : server.outputs) {
		if ((output_mask & output->bit)
				|| frame.intersects(output->viewport())) {
			output->damage();
		}
	}
}

void View::move(int new_x, int new_y) {
	if (x == new_x && y == new_y) {
		return;
	}
	damage();
	x = new_x;
	y = new_y;
	damage();
	server.visibility_dirty = true;
}

void View::close() {
	wlr_xdg_toplevel_send_close(wlr);
}

void View::activate() {
	if (!mapped) {
		return;
	}
	if (minimized) {
		set_minimized(false);
	}
	server.focus_view(this);
	server.reveal_view(*this);
}

void View::set_minimized(bool want) {
	if (minimized == want) {
		return;
	}
	minimized = want;
	if (want) {
		minimize_seq = ++server.minimize_counter;
		if (server.focused_view == this) {
			server.focus_view(server.top_visible_view(server.output_under_cursor()));
		}
	}
	if (foreign_) {
		wlr_foreign_toplevel_handle_v1_set_minimized(foreign_.get(), want);
	}
	damage();
	server.visibility_dirty = true;
	server.canvas.settle(*this);
}

void View::set_maximized(bool want) {
	if (maximized == want) {
		return;
	}
	resize_edges = 0; // a pending resize anchor must not fight the new box
	resize_end_serial = 0;
	if (want) {
		if (!maximized && !fullscreen) {
			saved = content_box();
		}
		maximized = true;
		if (fullscreen) {
			fullscreen = false;
			if (is_initialized(*this)) {
				wlr_xdg_toplevel_set_fullscreen(wlr, false);
			}
			if (foreign_) {
				wlr_foreign_toplevel_handle_v1_set_fullscreen(foreign_.get(), false);
			}
		}
		wlr_box area = maximize_area(*this);
		int tb = deco.titlebar();
		x = area.x;
		y = area.y + tb;
		if (is_initialized(*this)) {
			wlr_xdg_toplevel_set_maximized(wlr, true);
			wlr_xdg_toplevel_set_size(wlr, area.width, area.height - tb);
		}
	} else {
		maximized = false;
		if (is_initialized(*this)) {
			wlr_xdg_toplevel_set_maximized(wlr, false);
			if (!wlr_box_empty(&saved)) {
				wlr_xdg_toplevel_set_size(wlr, saved.width, saved.height);
				x = saved.x;
				y = saved.y;
			} else {
				wlr_xdg_toplevel_set_size(wlr, 0, 0);
			}
		}
	}
	if (foreign_) {
		wlr_foreign_toplevel_handle_v1_set_maximized(foreign_.get(), want);
	}
	damage();
	server.visibility_dirty = true;
	if (want || !is_initialized(*this)) {
		server.canvas.settle(*this);
	} else {
		// The restored size arrives with the client's next commit; settling on
		// the stale maximized geometry would lose its slice.
		settle_pending = true;
	}
}

void View::set_fullscreen(bool want) {
	if (fullscreen == want) {
		return;
	}
	resize_edges = 0;
	resize_end_serial = 0;
	if (want) {
		if (!maximized && !fullscreen) {
			saved = content_box();
		}
		fullscreen = true;
		if (maximized) {
			maximized = false;
			if (is_initialized(*this)) {
				wlr_xdg_toplevel_set_maximized(wlr, false);
			}
			if (foreign_) {
				wlr_foreign_toplevel_handle_v1_set_maximized(foreign_.get(), false);
			}
		}
		wlr_box area = fullscreen_area(*this);
		x = area.x;
		y = area.y;
		if (is_initialized(*this)) {
			wlr_xdg_toplevel_set_fullscreen(wlr, true);
			wlr_xdg_toplevel_set_size(wlr, area.width, area.height);
		}
		// Fullscreen windows sit above everything else.
		if (mapped) {
			std::erase(server.views, this);
			server.views.insert(server.views.begin(), this);
		}
	} else {
		fullscreen = false;
		if (is_initialized(*this)) {
			wlr_xdg_toplevel_set_fullscreen(wlr, false);
			if (!wlr_box_empty(&saved)) {
				wlr_xdg_toplevel_set_size(wlr, saved.width, saved.height);
				x = saved.x;
				y = saved.y;
			} else {
				wlr_xdg_toplevel_set_size(wlr, 0, 0);
			}
		}
	}
	if (foreign_) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(foreign_.get(), want);
	}
	damage();
	server.visibility_dirty = true;
	if (want || !is_initialized(*this)) {
		server.canvas.settle(*this);
	} else {
		// As with unmaximize: the board return waits for the size commit.
		settle_pending = true;
	}
}

// Grid placement: sized to the cell and told it is tiled.
void View::set_frame(wlr_box frame) {
	int tb = deco.titlebar();
	resize_edges = 0;
	resize_end_serial = 0;
	int bw = deco.border();
	wlr_box content{
		frame.x + bw,
		frame.y + tb + bw,
		frame.width - 2 * bw,
		frame.height - tb - 2 * bw,
	};
	if (content.width < 1) {
		content.width = 1;
	}
	if (content.height < 1) {
		content.height = 1;
	}
	bool was_maximized = maximized;
	bool was_fullscreen = fullscreen;
	maximized = false;
	fullscreen = false;
	if (is_initialized(*this)) {
		if (was_maximized) {
			wlr_xdg_toplevel_set_maximized(wlr, false);
		}
		if (was_fullscreen) {
			wlr_xdg_toplevel_set_fullscreen(wlr, false);
		}
		wlr_xdg_toplevel_set_tiled(wlr,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		wlr_xdg_toplevel_set_size(wlr, content.width, content.height);
	}
	if (foreign_) {
		if (was_maximized) {
			wlr_foreign_toplevel_handle_v1_set_maximized(foreign_.get(), false);
		}
		if (was_fullscreen) {
			wlr_foreign_toplevel_handle_v1_set_fullscreen(foreign_.get(), false);
		}
	}
	x = content.x;
	y = content.y;
	tiled = true;
	damage();
	server.visibility_dirty = true;
}

void View::set_tiled(bool want) {
	// Only the xdg state and the flag: joining and leaving boards is
	// Canvas::settle's business.
	if (tiled == want) {
		return;
	}
	tiled = want;
	if (is_initialized(*this)) {
		wlr_xdg_toplevel_set_tiled(wlr, want
			? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
			: 0);
	}
}

// ---- lookup ----

View *View::from_surface(wlr_surface *surface) {
	if (surface == nullptr) {
		return nullptr;
	}
	surface = wlr_surface_get_root_surface(surface);
	while (true) {
		wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
		if (popup == nullptr || popup->parent == nullptr) {
			break;
		}
		surface = wlr_surface_get_root_surface(popup->parent);
	}
	wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);
	if (toplevel == nullptr || toplevel->base == nullptr) {
		return nullptr;
	}
	return static_cast<View *>(toplevel->base->data);
}

void View::for_each_surface(
	std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const {
	wlr_xdg_surface_for_each_surface(
		wlr->base,
		[](wlr_surface *surface, int sx, int sy, void *data) {
			auto &callback =
				*static_cast<std::function_ref<void(wlr_surface *, int, int)> *>(data);
			callback(surface, sx, sy);
		},
		&fn);
}

// ---- foreign toplevel handle ----

void View::handle_foreign_activate() {
	activate();
}

void View::handle_foreign_maximize(
		const wlr_foreign_toplevel_handle_v1_maximized_event *event) {
	set_maximized(event->maximized);
}

void View::handle_foreign_minimize(
		const wlr_foreign_toplevel_handle_v1_minimized_event *event) {
	set_minimized(event->minimized);
}

void View::handle_foreign_fullscreen(
		const wlr_foreign_toplevel_handle_v1_fullscreen_event *event) {
	set_fullscreen(event->fullscreen);
}

void View::handle_foreign_close() {
	close();
}

void View::create_foreign() {
	wlr_foreign_toplevel_handle_v1 *handle =
		wlr_foreign_toplevel_handle_v1_create(server.foreign_mgr);
	if (handle == nullptr) {
		return;
	}
	foreign_.reset(handle);
	handle->data = this;
	if (wlr->title != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_title(handle, wlr->title);
	}
	if (wlr->app_id != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_app_id(handle, wlr->app_id);
	}
	if (maximized) {
		wlr_foreign_toplevel_handle_v1_set_maximized(handle, true);
	}
	if (minimized) {
		wlr_foreign_toplevel_handle_v1_set_minimized(handle, true);
	}
	if (fullscreen) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(handle, true);
	}
	foreign_activate_.connect<&View::handle_foreign_activate>(
		handle->events.request_activate, this);
	foreign_maximize_.connect<&View::handle_foreign_maximize>(
		handle->events.request_maximize, this);
	foreign_minimize_.connect<&View::handle_foreign_minimize>(
		handle->events.request_minimize, this);
	foreign_fullscreen_.connect<&View::handle_foreign_fullscreen>(
		handle->events.request_fullscreen, this);
	foreign_close_.connect<&View::handle_foreign_close>(
		handle->events.request_close, this);
}

void View::destroy_foreign() {
	if (!foreign_) {
		return;
	}
	foreign_activate_.disconnect();
	foreign_maximize_.disconnect();
	foreign_minimize_.disconnect();
	foreign_fullscreen_.disconnect();
	foreign_close_.disconnect();
	foreign_.reset();
}

void View::foreign_output_enter(Output &output) {
	if (foreign_) {
		wlr_foreign_toplevel_handle_v1_output_enter(foreign_.get(), output.wlr);
	}
}

void View::foreign_output_leave(Output &output) {
	if (foreign_) {
		wlr_foreign_toplevel_handle_v1_output_leave(foreign_.get(), output.wlr);
	}
}

// ---- toplevel events ----

View::View(Server &server, wlr_xdg_toplevel *wlr)
	: server(server), wlr(wlr), deco(*this) {
	wlr->base->data = this;
	map_.connect<&View::handle_map>(wlr->base->surface->events.map, this);
	unmap_.connect<&View::handle_unmap>(wlr->base->surface->events.unmap, this);
	commit_.connect<&View::handle_commit>(wlr->base->surface->events.commit, this);
	destroy_.connect<&View::handle_destroy>(wlr->events.destroy, this);
	request_move_.connect<&View::handle_request_move>(wlr->events.request_move, this);
	request_resize_.connect<&View::handle_request_resize>(
		wlr->events.request_resize, this);
	request_maximize_.connect<&View::handle_request_maximize>(
		wlr->events.request_maximize, this);
	request_minimize_.connect<&View::handle_request_minimize>(
		wlr->events.request_minimize, this);
	request_fullscreen_.connect<&View::handle_request_fullscreen>(
		wlr->events.request_fullscreen, this);
	set_title_.connect<&View::handle_set_title>(wlr->events.set_title, this);
	set_app_id_.connect<&View::handle_set_app_id>(wlr->events.set_app_id, this);
}

View::~View() = default;

void View::handle_map() {
	mapped = true;
	server.views.insert(server.views.begin(), this);
	deco.refresh_mode();
	place(*this);
	// A window opened on a tiled desktop joins its board.
	server.canvas.settle(*this);
	create_foreign();
	server.focus_view(this);
	server.visibility_dirty = true;
	damage();
}

void View::handle_unmap() {
	server.input.forget(*this);
	mapped = false;
	std::erase(server.views, this);
	destroy_foreign();
	if (server.focused_view == this) {
		server.focused_view = nullptr;
		server.focus_view(server.top_visible_view(server.output_under_cursor()));
	}
	// Reset state the client renegotiates when it remaps. The board needs no
	// re-layout: an unmapped window holds no slice, so its own frees up.
	maximized = false;
	fullscreen = false;
	tiled = false;
	board = std::nullopt;
	slice = std::nullopt;
	settle_pending = false;
	saved = wlr_box{0, 0, 0, 0};
	resize_edges = 0;
	deco.hover = DecoPart::none;
	deco.pressed = DecoPart::none;
	server.visibility_dirty = true;
	damage();
}

void View::handle_commit() {
	wlr_xdg_surface *base = wlr->base;
	if (base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(wlr,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		deco.handle_initial_commit();
		if (wlr->requested.fullscreen) {
			set_fullscreen(true);
		} else if (wlr->requested.maximized) {
			set_maximized(true);
		} else {
			wlr_xdg_toplevel_set_size(wlr, 0, 0);
		}
		return;
	}
	if (!mapped) {
		return;
	}
	if (resize_edges != 0) {
		// Interactive resize: keep the opposite edge anchored.
		wlr_box geo = base->geometry;
		if (resize_edges & WLR_EDGE_LEFT) {
			x = resize_start.x + resize_start.width - geo.width;
		}
		if (resize_edges & WLR_EDGE_TOP) {
			y = resize_start.y + resize_start.height - geo.height;
		}
		if (resize_end_serial != 0
				&& base->current.configure_serial >= resize_end_serial) {
			resize_edges = 0;
			resize_end_serial = 0;
		}
	}
	if (settle_pending) {
		settle_pending = false;
		server.canvas.settle(*this);
	}
	damage();
}

void View::handle_destroy() {
	server.input.forget(*this);
	if (server.focused_view == this) {
		server.focused_view = nullptr;
	}
	std::erase(server.views, this);
	destroy_foreign();
	destroy_owned(server.toplevels, this);
}

// ---- interactive move/resize requests ----

void View::handle_request_move() {
	if (!grab_allowed(server, *this)) {
		return;
	}
	Output *output = server.output_under_cursor();
	if (output != nullptr) {
		server.input.grab.begin_move(*this, *output);
	}
}

void View::handle_request_resize(wlr_xdg_toplevel_resize_event *event) {
	if (!grab_allowed(server, *this)) {
		return;
	}
	Output *output = server.output_under_cursor();
	if (output != nullptr) {
		server.input.grab.begin_resize(*this, *output, event->edges);
	}
}

void View::handle_request_maximize() {
	if (!is_initialized(*this)) {
		return; // the initial commit will honour the request
	}
	set_maximized(wlr->requested.maximized);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

void View::handle_request_minimize() {
	if (!is_initialized(*this)) {
		return;
	}
	set_minimized(wlr->requested.minimized);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

void View::handle_request_fullscreen() {
	if (!is_initialized(*this)) {
		return;
	}
	set_fullscreen(wlr->requested.fullscreen);
	wlr_xdg_surface_schedule_configure(wlr->base);
}

void View::handle_set_title() {
	if (foreign_ != nullptr && wlr->title != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_title(foreign_.get(), wlr->title);
	}
	damage();
}

void View::handle_set_app_id() {
	if (foreign_ != nullptr && wlr->app_id != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_app_id(foreign_.get(), wlr->app_id);
	}
	damage();
}

// ---- Server: views ----

void Server::init_views() {
	xdg_shell = wlr_xdg_shell_create(display, 6);
	new_xdg_toplevel_.connect<&Server::handle_new_xdg_toplevel>(
		xdg_shell->events.new_toplevel, this);
	new_xdg_popup_.connect<&Server::handle_new_xdg_popup>(
		xdg_shell->events.new_popup, this);
}

void Server::finish_views() {
	new_xdg_toplevel_.disconnect();
	new_xdg_popup_.disconnect();
	// Release textures and foreign toplevel handles before the renderer and display go.
	views.clear();
	toplevels.clear();
}

void Server::handle_new_xdg_toplevel(wlr_xdg_toplevel *toplevel) {
	toplevels.push_back(std::make_unique<View>(*this, toplevel));
}

void Server::handle_new_xdg_popup(wlr_xdg_popup *popup) {
	popups.push_back(std::make_unique<Popup>(*this, popup));
}

std::optional<ViewHit> Server::view_at(double cx, double cy, double zoom) {
	for (View *view : views) {
		if (!view->mapped || view->minimized) {
			continue;
		}
		wlr_box geo = view->wlr->base->geometry;
		// Canvas point -> toplevel surface-local coordinates.
		double lx = cx - (view->x - geo.x);
		double ly = cy - (view->y - geo.y);
		double sub_x = 0, sub_y = 0;
		wlr_surface *found = wlr_xdg_surface_surface_at(
			view->wlr->base, lx, ly, &sub_x, &sub_y);
		if (found != nullptr) {
			return ViewHit{view, found, sub_x, sub_y, DecoPart::none, 0};
		}
		DecoHit hit = view->deco.hit(cx, cy, zoom);
		if (hit.part != DecoPart::none) {
			return ViewHit{view, nullptr, 0, 0, hit.part, hit.edges};
		}
	}
	return std::nullopt;
}

View *Server::top_visible_view(Output *output) {
	for (View *view : views) {
		if (!view->mapped || view->minimized) {
			continue;
		}
		if (output == nullptr
				|| view->frame_rect().intersects(output->viewport())) {
			return view;
		}
	}
	return nullptr;
}

View *Server::last_minimized_view() {
	View *best = nullptr;
	for (View *view : views) {
		if (!view->minimized) {
			continue;
		}
		if (best == nullptr || view->minimize_seq > best->minimize_seq) {
			best = view;
		}
	}
	return best;
}

void Server::focus_view(View *view) {
	View *prev = focused_view;
	if (prev != view) {
		if (prev != nullptr) {
			wlr_xdg_toplevel_set_activated(prev->wlr, false);
			if (prev->foreign_) {
				wlr_foreign_toplevel_handle_v1_set_activated(prev->foreign_.get(), false);
			}
			prev->damage();
		}
		focused_view = view;
		if (view != nullptr) {
			// Raise to the top of the stacking order.
			std::erase(views, view);
			views.insert(views.begin(), view);
			wlr_xdg_toplevel_set_activated(view->wlr, true);
			if (view->foreign_) {
				wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_.get(), true);
			}
			view->damage();
		}
	}
	// The keyboard follows the focused window. An exclusive layer surface
	// (a lock screen) keeps it; an on-demand one (a panel) gives it up to a
	// window being focused, the one already focused behind it included.
	if (view != nullptr && focused_layer != nullptr
			&& !focused_layer->holds_keyboard_exclusively()) {
		focused_layer = nullptr;
	}
	if (focused_layer == nullptr) {
		focus_keyboard(view != nullptr ? view->wlr->base->surface : nullptr);
	}
}

void Server::focus_keyboard(wlr_surface *surface) {
	if (surface == nullptr) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	// With no keyboard yet the focus is still recorded: wlroots enters the
	// surface once its client binds the first keyboard that appears.
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard == nullptr) {
		wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
		return;
	}
	wlr_seat_keyboard_notify_enter(seat, surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

// ---- popups ----

Popup::Popup(Server &server, wlr_xdg_popup *wlr)
	: wlr(wlr), server_(server) {
	commit_.connect<&Popup::handle_commit>(wlr->base->surface->events.commit, this);
	destroy_.connect<&Popup::handle_destroy>(wlr->events.destroy, this);
}

Popup::~Popup() = default;

void Popup::handle_commit() {
	if (wlr->base->initial_commit) {
		if (!popup_unconstrain(*this)) {
			wlr_xdg_surface_schedule_configure(wlr->base);
		}
	}
	// Later commits damage through server.cpp's per-surface tracker.
}

void Popup::handle_destroy() {
	destroy_owned(server_.popups, this);
}
