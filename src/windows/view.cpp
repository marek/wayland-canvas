#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <string>
#include "core/box.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "core/server.h"
#if CANVAS_HAVE_XWAYLAND
#include "windows/view_x11.h"
#endif
#include "windows/view_xdg.h"

namespace {

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

// ---- placement ----

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

// Whether an output shows the window: an open overview shows the canvas, a
// normal view the windows whose frame centre its viewport holds.
bool shown_anywhere(const View &view) {
	Rect frame = view.frame_rect();
	return view.server.output_showing(frame.cx(), frame.cy()) != nullptr
		|| std::ranges::any_of(view.server.outputs,
			[](const std::unique_ptr<Output> &output) { return output->overview.active(); });
}

// For the hint about a window that opened unseen: where it is.
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
	wlr_box geo = view.shell_geometry();
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
	// A dialog sits over the window it belongs to, its title bar never above
	// that window's.
	View *parent = opens_over_parent(view.kind()) ? view.shell_parent() : nullptr;
	if (parent != nullptr && parent->mapped && !parent->minimized) {
		wlr_box over = parent->frame_box();
		double px = over.x + (over.width - fw) / 2.0;
		double py = std::fmax(over.y, over.y + (over.height - fh) / 2.0);
		view.x = static_cast<int>(std::round(px)) + bw;
		view.y = static_cast<int>(std::round(py)) + tb + bw;
		return;
	}

	// Inside the screen minus bars (exclusive zones), at the zoom the view
	// rests at there.
	Opening opening = opening_of(view, *output);
	Rect area = rect_of_box(opening.output->usable_canvas_box(opening.box, opening.zoom));
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
	int left = static_cast<int>(std::round(px));
	int top = static_cast<int>(std::round(py));
	int x = left + bw;
	int y = top + tb + bw;

	// Cascade by a step while another view occupies the chosen position, as
	// far as the frame stays inside the area; with every position that far
	// taken, the window opens on the first again.
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
	double room = std::fmin(area.x + area.w - (left + fw), area.y + area.h - (top + fh));
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

bool is_initialized(const View &view) {
	return view.initialized();
}

// ---- interactive move/resize requests ----

// Only honour requests stemming from a real button press on the focused
// window; anything else is a client trying its luck.
bool grab_allowed(const Server &server, const View &view) {
	return server.focused_view == &view
		&& !server.input.grab.active()
		&& server.seat->pointer_state.button_count > 0;
}

} // namespace

// ---- geometry ----

wlr_box View::content_box() const {
	wlr_box geo = shell_geometry();
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

Point View::surface_origin() const {
	wlr_box geo = shell_geometry();
	if (wlr_box_empty(&geo)) {
		return Point{static_cast<double>(x), static_cast<double>(y)};
	}
	return Point{static_cast<double>(x - geo.x), static_cast<double>(y - geo.y)};
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
	server.windows_dirty = true;
}

void View::close() {
	send_close();
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
	server.windows_dirty = true;
	server.canvas.settle(*this);
}

void View::set_maximized(bool want) {
	if (maximized == want) {
		return;
	}
	drop_resize(); // a pending resize anchor must not fight the new box
	if (want) {
		if (!maximized && !fullscreen) {
			saved = content_box();
		}
		maximized = true;
		if (fullscreen) {
			fullscreen = false;
			if (is_initialized(*this)) {
				tell_fullscreen(false);
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
			tell_maximized(true);
			configure_size(area.width, area.height - tb);
		}
	} else {
		maximized = false;
		if (is_initialized(*this)) {
			tell_maximized(false);
			if (!wlr_box_empty(&saved)) {
				configure_size(saved.width, saved.height);
				x = saved.x;
				y = saved.y;
			} else {
				configure_size(0, 0);
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
	drop_resize();
	if (want) {
		if (!maximized && !fullscreen) {
			saved = content_box();
		}
		fullscreen = true;
		if (maximized) {
			maximized = false;
			if (is_initialized(*this)) {
				tell_maximized(false);
			}
			if (foreign_) {
				wlr_foreign_toplevel_handle_v1_set_maximized(foreign_.get(), false);
			}
		}
		// Over other windows it goes to a desktop of its own and covers that
		// (canvas/fullscreen_desktops.h).
		server.canvas.fullscreen_desktops.enter(*this, fullscreen_area(*this));
		wlr_box area = fullscreen_area(*this);
		x = area.x;
		y = area.y;
		if (is_initialized(*this)) {
			tell_fullscreen(true);
			configure_size(area.width, area.height);
		}
		// Fullscreen windows sit above everything else.
		if (mapped) {
			std::erase(server.views, this);
			server.views.insert(server.views.begin(), this);
		}
	} else {
		fullscreen = false;
		if (is_initialized(*this)) {
			tell_fullscreen(false);
			if (!wlr_box_empty(&saved)) {
				configure_size(saved.width, saved.height);
				x = saved.x;
				y = saved.y;
			} else {
				configure_size(0, 0);
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
	drop_resize();
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
			tell_maximized(false);
		}
		if (was_fullscreen) {
			tell_fullscreen(false);
		}
		tell_tiled(true);
		configure_size(content.width, content.height);
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
		tell_tiled(want);
	}
}

// ---- lookup ----

View *View::from_surface(wlr_surface *surface) {
	if (surface == nullptr) {
		return nullptr;
	}
	if (XdgView *view = XdgView::from_surface(surface)) {
		return view;
	}
#if CANVAS_HAVE_XWAYLAND
	return X11View::from_surface(surface);
#else
	return nullptr;
#endif
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
	if (shell_title() != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_title(handle, shell_title());
	}
	if (shell_app_id() != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_app_id(handle, shell_app_id());
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
	// The ext-foreign-toplevel-list handle names the window for capture
	// clients (image_capture.cpp reads it back through ->data).
	if (server.image_capture.list != nullptr) {
		wlr_ext_foreign_toplevel_handle_v1_state state{
			.title = shell_title(), .app_id = shell_app_id()};
		wlr_ext_foreign_toplevel_handle_v1 *ext =
			wlr_ext_foreign_toplevel_handle_v1_create(server.image_capture.list, &state);
		if (ext != nullptr) {
			ext_foreign_.reset(ext);
			ext->data = this;
		}
	}
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
	ext_foreign_.reset();
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

const char *View::ext_toplevel_identifier() const {
	return ext_foreign_ != nullptr ? ext_foreign_->identifier : nullptr;
}

// ---- toplevel events ----

View::View(Server &server)
	: server(server), deco(*this) {
	static std::atomic<uint64_t> next_session_id = 1;
	session_id = next_session_id++;
}

View::~View() = default;

void View::handle_map() {
	mapped = true;
	laid_out_kind_ = kind();
	server.views.insert(server.views.begin(), this);
	deco.refresh_mode();
	if (placed_by_client(kind())) {
		// A menu or a tooltip belongs where its client put it, beside the
		// window it hangs from; it joins no board and takes no focus
		// unless it asks for one.
		server.visibility_dirty = true;
		damage();
		return;
	}
	place(*this);
	// A window opened on a tiled desktop joins its board.
	server.canvas.settle(*this);
	create_foreign();
	if (launched_for.has_value() && !shown_anywhere(*this)) {
		// Launched for a spot no output shows any more (the overview closed
		// or moved on before it appeared): it opens there without taking the
		// keyboard from the window in front of the user, and a hint says where.
		server.hint.set(opened_where(*this));
	} else {
		server.focus_view(this);
	}
	launched_for.reset();
	server.visibility_dirty = true;
	server.windows_dirty = true;
	damage();
}

void Server::view_gone(View &view) {
	input.forget(view);
	capture_ui.forget(view);
	image_capture.forget(view);
}

void View::handle_unmap() {
	server.view_gone(*this);
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
	drop_resize();
	deco.hover = DecoPart::none;
	deco.pressed = DecoPart::none;
	server.visibility_dirty = true;
	server.windows_dirty = true;
	damage();
}

// The client's first commit, whichever shell it speaks: the window learns
// where it will open and what state it asked to open in.
void View::handle_first_commit() {
	deco.handle_initial_commit();
	launched_for = server.launches.spot_for(client_pid());
	// Until it maps, the frame waits where the window will open: a request
	// to start maximized or fullscreen, sent before this commit or after
	// it (foot asks after), fills the area around that spot.
	place(*this);
	if (wants_fullscreen()) {
		set_fullscreen(true);
	} else if (wants_maximized()) {
		set_maximized(true);
	} else {
		configure_size(0, 0);
	}
}

void View::handle_kind_change() {
	ViewKind before = laid_out_kind_, now = kind();
	if (!mapped || now == before) {
		return;
	}
	laid_out_kind_ = now;
	damage();
	if (!tiles(now)) {
		server.canvas.leave_board(*this);
	}
	// A window that stops being a dialog stays where it is.
	if (opens_over_parent(now) && !opens_over_parent(before)) {
		place(*this);
	}
	server.canvas.settle(*this);
	server.visibility_dirty = true;
	server.windows_dirty = true;
	damage();
}

void View::handle_commit() {
	if (!mapped) {
		return;
	}
	if (settle_pending) {
		settle_pending = false;
		server.canvas.settle(*this);
	}
	damage();
}

void View::handle_destroy() {
	server.view_gone(*this);
	if (server.focused_view == this) {
		server.focused_view = nullptr;
	}
	std::erase(server.views, this);
	destroy_foreign();
	server.windows_dirty = true;
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

void View::handle_request_resize(uint32_t edges) {
	if (!grab_allowed(server, *this)) {
		return;
	}
	Output *output = server.output_under_cursor();
	if (output != nullptr) {
		server.input.grab.begin_resize(*this, *output, edges);
	}
}

void View::handle_set_title() {
	if (foreign_ != nullptr && shell_title() != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_title(foreign_.get(), shell_title());
	}
	if (ext_foreign_ != nullptr) {
		wlr_ext_foreign_toplevel_handle_v1_state state{
			.title = shell_title(), .app_id = shell_app_id()};
		wlr_ext_foreign_toplevel_handle_v1_update_state(ext_foreign_.get(), &state);
	}
	server.windows_dirty = true;
	damage();
}

void View::handle_set_app_id() {
	if (foreign_ != nullptr && shell_app_id() != nullptr) {
		wlr_foreign_toplevel_handle_v1_set_app_id(foreign_.get(), shell_app_id());
	}
	if (ext_foreign_ != nullptr) {
		wlr_ext_foreign_toplevel_handle_v1_state state{
			.title = shell_title(), .app_id = shell_app_id()};
		wlr_ext_foreign_toplevel_handle_v1_update_state(ext_foreign_.get(), &state);
	}
	server.windows_dirty = true;
	damage();
}

// ---- Server: views ----

void Server::finish_views() {
	// Release textures and foreign toplevel handles before the renderer and display go.
	views.clear();
	toplevels.clear();
}

std::optional<ViewHit> Server::view_at(double cx, double cy, double zoom) {
	for (View *view : views) {
		if (!view->mapped || view->minimized) {
			continue;
		}
		wlr_box geo = view->shell_geometry();
		// Canvas point -> toplevel surface-local coordinates.
		double lx = cx - (view->x - geo.x);
		double ly = cy - (view->y - geo.y);
		double sub_x = 0, sub_y = 0;
		wlr_surface *found = view->surface_at(lx, ly, &sub_x, &sub_y);
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
		if (!view->mapped || view->minimized || !takes_focus(view->kind())) {
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
		windows_dirty = true;
		if (prev != nullptr) {
			prev->tell_activated(false);
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
			view->tell_activated(true);
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
		focus_keyboard(view != nullptr ? view->surface() : nullptr);
	}
}

void Server::focus_in(wlr_box box) {
	if (focused_view != nullptr) {
		Rect frame = focused_view->frame_rect();
		if (wlr_box_contains_point(&box, frame.cx(), frame.cy())) {
			return;
		}
	}
	for (View *view : canvas.views_in(box)) {
		if (takes_focus(view->kind())) {
			focus_view(view);
			return;
		}
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
