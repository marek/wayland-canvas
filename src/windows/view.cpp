#include <atomic>
#include "core/box.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "core/server.h"
#include "windows/placement.h"
#include "windows/view_x11.h"
#include "windows/view_xdg.h"

namespace {

// The mode a window ends in when `off` is switched off: normal when it is in
// that mode, else the one it is in.
WindowMode without(WindowMode current, WindowMode off) {
	return current == off ? WindowMode::normal : current;
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

void View::visibility_changed() {
	damage();
	server.visibility_changed();
}

void View::update_outputs(uint32_t mask) {
	uint32_t old = output_mask;
	output_mask = mask;
	// The scale the view's clients render for: the largest scale of the
	// outputs it is on. A desktop's scale is the compositor's scaling of the
	// picture and never reaches them, so a zoom never re-renders or resizes
	// a window.
	double scale = 0;
	for (const std::unique_ptr<Output> &output : server.outputs) {
		if (mask & output->bit) {
			scale = Output::sharper_scale(scale, *output->wlr);
		}
	}
	// Like wlr_scene: a view scrolled off every output keeps its last
	// outputs, so panning away and back never makes clients re-render.
	if (mask != 0 && mask != old) {
		for_each_surface([&](wlr_surface *surface, int, int) {
			for (const std::unique_ptr<Output> &output : server.outputs) {
				if (mask & output->bit) {
					wlr_surface_send_enter(surface, output->wlr);
				} else {
					wlr_surface_send_leave(surface, output->wlr);
				}
			}
			Output::tell_scale(surface, scale);
		});
		told_scale = scale;
	} else if (mask != 0 && scale != told_scale) {
		// Same outputs, a new scale: one of them changed its scale.
		for_each_surface([&](wlr_surface *surface, int, int) {
			Output::tell_scale(surface, scale);
		});
		told_scale = scale;
	}
	for (const std::unique_ptr<Output> &output : server.outputs) {
		bool was = old & output->bit, is = mask & output->bit;
		if (is && !was) {
			foreign_.output_enter(*output);
		} else if (was && !is) {
			foreign_.output_leave(*output);
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
	visibility_changed();
	server.changed(Change::windows);
}

void View::translate(int dx, int dy) {
	move(x + dx, y + dy);
	saved.x += dx;
	saved.y += dy;
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
	foreign_.publish();
	visibility_changed();
	server.changed(Change::windows);
	server.canvas.settle(*this);
}

void View::set_maximized(bool want) {
	set_mode(want ? WindowMode::maximized : without(mode, WindowMode::maximized));
}

void View::set_fullscreen(bool want) {
	set_mode(want ? WindowMode::fullscreen : without(mode, WindowMode::fullscreen));
}

void View::set_mode(WindowMode want) {
	if (mode == want) {
		return;
	}
	drop_resize(); // a pending resize anchor must not fight the new box
	if (mode == WindowMode::normal) {
		saved = content_box();
	}
	switch_mode(want);
	if (want == WindowMode::fullscreen) {
		// Over other windows it goes to a desktop of its own and covers that
		// (canvas/fullscreen_desktops.h).
		server.canvas.fullscreen_desktops.enter(*this, fullscreen_area(*this));
		// Fullscreen windows sit above everything else.
		if (mapped) {
			std::erase(server.views, this);
			server.views.insert(server.views.begin(), this);
		}
	}
	if (std::optional<wlr_box> area = area_for(*this, want)) {
		int tb = deco.titlebar();
		x = area->x;
		y = area->y + tb;
		if (is_initialized(*this)) {
			configure_size(area->width, area->height - tb);
		}
	} else if (is_initialized(*this)) {
		if (!wlr_box_empty(&saved)) {
			configure_size(saved.width, saved.height);
			x = saved.x;
			y = saved.y;
		} else {
			configure_size(0, 0);
		}
	}
	visibility_changed();
	if (want != WindowMode::normal || !is_initialized(*this)) {
		server.canvas.settle(*this);
	} else {
		// The restored size arrives with the client's next commit; settling on
		// the stale maximized or fullscreen geometry would lose its slice.
		settle_pending = true;
	}
}

void View::switch_mode(WindowMode want) {
	WindowMode was = mode;
	mode = want;
	announce_mode(was, false);
	announce_mode(want, true);
	foreign_.publish();
}

void View::announce_mode(WindowMode of, bool on) {
	if (!is_initialized(*this)) {
		return;
	}
	if (of == WindowMode::maximized) {
		tell_maximized(on);
	} else if (of == WindowMode::fullscreen) {
		tell_fullscreen(on);
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
	switch_mode(WindowMode::normal);
	if (is_initialized(*this)) {
		tell_tiled(true);
		configure_size(content.width, content.height);
	}
	x = content.x;
	y = content.y;
	tiled = true;
	visibility_changed();
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
	return X11View::from_surface(surface);
}

const char *View::ext_toplevel_identifier() const {
	return foreign_.identifier();
}

// ---- toplevel events ----

View::View(Server &server)
	: server(server), deco(*this), foreign_(*this) {
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
		visibility_changed();
		return;
	}
	place(*this);
	// A window opened on a tiled desktop joins its board.
	server.canvas.settle(*this);
	foreign_.open();
	if (launched_for.has_value() && !shown_anywhere(*this)) {
		// Launched for a spot no output shows any more (the overview closed
		// or moved on before it appeared): it opens there without taking the
		// keyboard from the window in front of the user, and a hint says where.
		server.hint.set(opened_where(*this));
	} else {
		server.focus_view(this);
	}
	launched_for.reset();
	visibility_changed();
	server.changed(Change::windows);
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
	foreign_.close();
	if (server.focused_view == this) {
		server.focused_view = nullptr;
		server.focus_view(server.top_visible_view(server.output_under_cursor()));
	}
	// Reset state the client renegotiates when it remaps. The board needs no
	// re-layout: an unmapped window holds no slice, so its own frees up.
	mode = WindowMode::normal;
	tiled = false;
	board_seat.reset();
	settle_pending = false;
	saved = wlr_box{0, 0, 0, 0};
	drop_resize();
	deco.set_hover(DecoPart::none);
	deco.set_pressed(DecoPart::none);
	visibility_changed();
	server.changed(Change::windows);
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
	server.changed(Change::windows);
	visibility_changed();
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
	foreign_.close();
	server.changed(Change::windows);
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

void View::handle_set_names() {
	foreign_.publish();
	server.changed(Change::windows);
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
		changed(Change::windows);
		focused_view = view;
		if (prev != nullptr) {
			prev->tell_activated(false);
			prev->foreign_.publish();
			prev->damage();
		}
		if (view != nullptr) {
			// Raise to the top of the stacking order.
			std::erase(views, view);
			views.insert(views.begin(), view);
			view->tell_activated(true);
			view->foreign_.publish();
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
		: centred_box(output->width, output->height, centre.x, centre.y);
	if (output->overview.active()) {
		output->overview.set_target(box, id_of(desktop), false);
		output->overview.exit();
		return;
	}
	output->camera.animate_to(
		camera_for_box(box, desktop != nullptr ? desktop->scale() : 1.0));
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
