#include "input/pointer_router.h"

#include <cmath>
#include <optional>
#include <variant>
#include <linux/input-event-codes.h>
#include "canvas/overview_chrome.h"
#include "core/config.h"
#include "core/server.h"
#include "input/ui_layer.h"
#include "platform/output.h"

namespace {

// The bit `button` holds in the buttons pressed and forwarded to a client.
uint32_t button_bit(uint32_t button) {
	return 1u << (button & 31);
}

// How many surface-local px one output px is on a surface: one on the plane
// is measured in canvas px, at the output's zoom; any other is at 1.
double surface_scale(const Output &output, const View *view,
		const LayerSurface *layer) {
	bool on_plane = view != nullptr
		|| (layer != nullptr && layer->canvas_box.has_value());
	return on_plane ? 1.0 / output.camera.drawn().zoom : 1.0;
}

// Hover bookkeeping without extra server state: clear every view's hover
// except the one under the pointer. Returns true when anything changed.
bool set_view_hover(Server &server, View *hover, DecoPart part) {
	bool changed = false;
	for (View *view : server.views) {
		DecoPart want = view == hover ? part : DecoPart::none;
		if (view->deco.set_hover(want)) {
			changed = true;
		}
	}
	return changed;
}

struct PointerHit {
	wlr_surface *surface = nullptr; // client surface under the pointer
	View *view = nullptr;            // view frame under the pointer (or null)
	LayerSurface *layer = nullptr;   // layer surface's owner (or null)
	double sx = 0, sy = 0;           // surface-local coordinates
	DecoPart part = DecoPart::none;  // decoration part when surface == null
	uint32_t edges = 0;              // resize edges for DecoPart::resize
};

// The shell surface (top or overlay layer: the bar, panels, the toolbar)
// at output-local (ox, oy), if any.
std::optional<LayerHit> shell_surface_at(Output &output, double ox, double oy) {
	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			l >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; l--) {
		if (std::optional<LayerHit> hit = output.layer_surface_at(l, ox, oy)) {
			return hit;
		}
	}
	// A surface a plugin put on the plane is hit where it is drawn, which
	// is the canvas's business, not the screen's.
	return output.canvas_surface_at(ox, oy);
}

// The shell surface under the cursor.
std::optional<LayerHit> shell_surface_at(Server &server, Output &output) {
	return shell_surface_at(output, server.cursor->x - output.lx,
		server.cursor->y - output.ly);
}

// A click on a shell surface that takes the keyboard offers it the keyboard.
// A surface on the plane comes up above the others there, as a clicked
// window does.
void focus_layer_on_click(Server &server, LayerSurface &layer, wlr_surface *surface) {
	if (layer.canvas_box.has_value()) {
		server.canvas_surfaces.raise(layer);
	}
	if (layer.wants_keyboard()) {
		server.focus.offer_keyboard(layer, surface);
	}
}

// A press on the empty canvas takes the keyboard from a shell surface that
// holds it on demand (a note being written in) back to the focused window.
void press_on_empty_canvas(Server &server) {
	LayerSurface *holder = server.focus.focused_layer();
	if (holder != nullptr && !holder->holds_keyboard_exclusively()) {
		server.focus.return_keyboard();
	}
}

PointerHit pointer_hit_test(Server &server, Output &output, double ox, double oy) {
	PointerHit hit;

	if (std::optional<LayerHit> shell = shell_surface_at(output, ox, oy)) {
		hit.surface = shell->surface;
		hit.layer = shell->layer;
		hit.sx = shell->sx;
		hit.sy = shell->sy;
		return hit;
	}

	Point p = output.local_to_canvas(ox, oy);

	std::optional<ViewHit> view_hit =
		server.focus.view_at(p.x, p.y, output.camera.drawn().zoom);
	if (view_hit.has_value()) {
		hit.view = view_hit->view;
		hit.surface = view_hit->surface;
		hit.sx = view_hit->sx;
		hit.sy = view_hit->sy;
		hit.part = view_hit->part;
		hit.edges = view_hit->edges;
		return hit;
	}

	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
			l >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; l--) {
		std::optional<LayerHit> layer_hit = output.layer_surface_at(l, ox, oy);
		if (layer_hit.has_value()) {
			hit.surface = layer_hit->surface;
			hit.layer = layer_hit->layer;
			hit.sx = layer_hit->sx;
			hit.sy = layer_hit->sy;
			return hit;
		}
	}
	return hit;
}

// What is under the cursor.
PointerHit pointer_hit_test(Server &server, Output &output) {
	return pointer_hit_test(server, output, server.cursor->x - output.lx,
		server.cursor->y - output.ly);
}

uint32_t quadrant_edges(View &view, double cx, double cy) {
	wlr_box frame = view.frame_box();
	uint32_t edges = 0;
	edges |= (cx - frame.x) * 2 < frame.width ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
	edges |= (cy - frame.y) * 2 < frame.height ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
	return edges;
}

// What an overview press landed on, following the render order: shelf
// item, tab buttons, tab body, window, desktop interior,
// empty.
PressTarget overview_press_target(Server &server, Output &output, double ox,
		double oy, double cx, double cy, double zoom) {
	Shelf shelf = overview_shelf_layout(output);
	for (const ShelfItem &item : shelf) {
		if (wlr_box_contains_point(&item.box, ox, oy)) {
			return PressOnShelf{.view = item.view};
		}
	}

	for (const Desktop &desktop : server.canvas.desktops.all()) {
		std::optional<TabLayout> tab = overview_tab_layout(output, desktop);
		if (!tab.has_value()) {
			continue;
		}
		if (wlr_box_contains_point(&tab->tile_button, ox, oy)) {
			return PressOnTile{.desktop = desktop.id()};
		}
		if (wlr_box_contains_point(&tab->abandon_button, ox, oy)) {
			return PressOnAbandon{.desktop = desktop.id()};
		}
		if (wlr_box_contains_point(&tab->tab, ox, oy)) {
			return PressOnDesktop{.desktop = desktop.id()};
		}
	}


	std::optional<ViewHit> hit = server.focus.view_at(cx, cy, zoom);
	if (hit.has_value()) {
		return PressOnView{.view = hit->view};
	}

	if (Desktop *desktop = server.canvas.desktops.at(cx, cy)) {
		return PressOnDesktop{.desktop = desktop->id()};
	}
	return PressOnEmpty{};
}

// The desktop a press landed on: its tab (the body or a button) or its
// interior clear of windows.
std::optional<DesktopId> pressed_desktop(const PressTarget &target) {
	if (const auto *on = std::get_if<PressOnDesktop>(&target)) {
		return on->desktop;
	}
	if (const auto *on = std::get_if<PressOnTile>(&target)) {
		return on->desktop;
	}
	if (const auto *on = std::get_if<PressOnAbandon>(&target)) {
		return on->desktop;
	}
	return std::nullopt;
}

// A right click removes the desktop it lands on (its windows stay on the
// canvas) and makes one on the empty canvas; on a window or the
// shelf it does nothing.
void overview_right_click(Output &output, const PressTarget &target, Point p) {
	if (std::optional<DesktopId> desktop = pressed_desktop(target)) {
		output.server.canvas.abandon(desktop);
		return;
	}
	if (std::holds_alternative<PressOnEmpty>(target)) {
		output.overview.new_desktop(p.x, p.y);
	}
}

// How far, in screen px, a scroll looks away: by the pan switch alone,
// whichever way the device scrolls apps. libinput turns a natural device's
// delta round and says so in relative_direction, so it is turned back first.
double pan_delta(const Server &server, const wlr_pointer_axis_event &event) {
	double traditional = event.relative_direction == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
		? -event.delta : event.delta;
	return (server.conf.resolved.pan_traditional ? traditional : -traditional)
		* config::scroll_pan_speed;
}

} // namespace

PointerRouter::PointerRouter(Server &server, Input &input)
	: server_(server), input_(input) {}

void PointerRouter::forget(const View &view) {
	if (last_title_view == &view) {
		last_title_view = nullptr;
	}
}

// While locked the pointer belongs to the locker's surface for the output
// it is over, and to nothing else.
bool PointerRouter::locked_pointer_focus(uint32_t time) {
	Server &server = server_;
	if (!server.input.gate().locked()) {
		return false;
	}
	Output *output = server.outputs.at(server.cursor->x, server.cursor->y);
	wlr_surface *surface = output != nullptr ? server.input.gate().surface_for(*output)
		: nullptr;
	if (surface == nullptr) {
		wlr_seat_pointer_notify_clear_focus(server.seat);
		return true;
	}
	double sx = server.cursor->x - output->lx, sy = server.cursor->y - output->ly;
	wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
	return true;
}

void PointerRouter::motion(uint32_t time) {
	Server &server = server_;
	if (locked_pointer_focus(time)) {
		input_.set_cursor_image("default");
		return;
	}
	// X's picture of its windows keeps up with the pointer wherever it is.
	server.xwm.follow_pointer();
	if (input_.grab.active()) {
		input_.grab.motion();
		return;
	}
	if (held_motion(time)) {
		return;
	}

	Output *output = server.outputs.at(server.cursor->x, server.cursor->y);
	if (output == nullptr) {
		wlr_seat_pointer_notify_clear_focus(server.seat);
		input_.set_cursor_image("default");
		return;
	}
	double ox = server.cursor->x - output->lx, oy = server.cursor->y - output->ly;
	for (UiLayer *layer : input_.layers()) {
		if (layer->pointer_motion(*output, ox, oy)) {
			wlr_seat_pointer_notify_clear_focus(server.seat);
			set_view_hover(server, nullptr, DecoPart::none);
			input_.set_cursor_image(layer->cursor_image());
			return;
		}
	}

	if (!output->interactive()) {
		// Shell surfaces (the bar, panels, the toolbar) keep the pointer in
		// the overview and during transitions.
		if (std::optional<LayerHit> shell = shell_surface_at(server, *output)) {
			if (set_view_hover(server, nullptr, DecoPart::none)) {
				output->damage();
			}
			wlr_seat_pointer_notify_enter(server.seat, shell->surface, shell->sx, shell->sy);
			wlr_seat_pointer_notify_motion(server.seat, time, shell->sx, shell->sy);
			return;
		}
		// Overview / animating: the compositor owns the pointer. Keep the
		// title bar hover state of the view under the cursor alive so the
		// buttons light up, but no client sees any event.
		wlr_seat_pointer_notify_clear_focus(server.seat);
		Camera cam = output->camera.drawn();
		Point p = output->local_to_canvas(server.cursor->x - output->lx,
			server.cursor->y - output->ly);
		std::optional<ViewHit> hit = server.focus.view_at(p.x, p.y, cam.zoom);
		DecoPart part = hit ? hit->part : DecoPart::none;
		View *view = hit ? hit->view : nullptr;
		wlr_surface *surface = hit ? hit->surface : nullptr;
		if (set_view_hover(server, surface != nullptr ? nullptr : view, part)) {
			output->damage();
		}
		input_.set_cursor_image("default");
		return;
	}

	PointerHit hit = pointer_hit_test(server, *output);
	if (hit.surface != nullptr) {
		set_view_hover(server, nullptr, DecoPart::none);
		wlr_seat_pointer_notify_enter(server.seat, hit.surface, hit.sx, hit.sy);
		wlr_seat_pointer_notify_motion(server.seat, time, hit.sx, hit.sy);
		// The client owns the cursor image while it has pointer focus.
	} else {
		wlr_seat_pointer_notify_clear_focus(server.seat);
		if (hit.view != nullptr) {
			set_view_hover(server, hit.view, hit.part);
			input_.set_cursor_image(hit.part == DecoPart::resize
				? wlr_xcursor_get_resize_name(static_cast<wlr_edges>(hit.edges))
					: "default");
		} else {
			set_view_hover(server, nullptr, DecoPart::none);
			input_.set_cursor_image("default");
		}
	}
}

void PointerRouter::overview_press(Output &output, uint32_t button) {
	Camera cam = output.camera.drawn();
	double ox = server_.cursor->x - output.lx;
	double oy = server_.cursor->y - output.ly;
	Point p = output.local_to_canvas(ox, oy);

	if (button != BTN_LEFT && button != BTN_RIGHT) {
		return;
	}
	PressTarget target =
		overview_press_target(server_, output, ox, oy, p.x, p.y, cam.zoom);
	if (button == BTN_RIGHT) {
		overview_right_click(output, target, p);
		return;
	}
	if (std::holds_alternative<PressOnEmpty>(target)) {
		press_on_empty_canvas(server_);
	}
	input_.grab.begin_overview_press(output, target);
}

void PointerRouter::forward_press(uint32_t time, uint32_t button, wlr_surface *surface,
		double sx, double sy, double scale) {
	if (pressed_buttons_ == 0) {
		held_ = HeldPress{.surface = surface, .sx = sx, .sy = sy,
			.lx = server_.cursor->x, .ly = server_.cursor->y, .scale = scale};
		held_destroy_.disconnect();
		held_destroy_.connect<&PointerRouter::handle_held_destroy>(surface->events.destroy,
			this);
	}
	pressed_buttons_ |= button_bit(button);
	// The press goes where it landed, though no motion entered it there.
	wlr_seat_pointer_notify_enter(server_.seat, surface, sx, sy);
	wlr_seat_pointer_notify_button(server_.seat, time, button,
		WL_POINTER_BUTTON_STATE_PRESSED);
}

bool PointerRouter::held_motion(uint32_t time) {
	// A drag-and-drop the client started routes the pointer itself, from
	// surface to surface.
	if (held_.surface == nullptr || pressed_buttons_ == 0
			|| server_.seat->drag != nullptr) {
		return false;
	}
	wlr_seat_pointer_notify_motion(server_.seat, time,
		held_.sx + (server_.cursor->x - held_.lx) * held_.scale,
		held_.sy + (server_.cursor->y - held_.ly) * held_.scale);
	return true;
}

void PointerRouter::end_hold() {
	held_ = HeldPress{};
	held_destroy_.disconnect();
}

void PointerRouter::handle_held_destroy() {
	end_hold();
}

bool PointerRouter::button_held_on(const wlr_surface *surface) const {
	wlr_surface *focus = server_.seat->pointer_state.focused_surface;
	return pressed_buttons_ != 0 && focus != nullptr
		&& wlr_surface_get_root_surface(focus) == surface;
}

void PointerRouter::button_press(uint32_t time, uint32_t button) {
	Server &server = server_;
	input_.super_tap.other_input();
	if (server.input.gate().locked()) {
		if (server.seat->pointer_state.focused_surface != nullptr) {
			pressed_buttons_ |= button_bit(button);
			wlr_seat_pointer_notify_button(server.seat, time, button,
				WL_POINTER_BUTTON_STATE_PRESSED);
		}
		return;
	}
	if (input_.grab.active()) {
		return; // extra buttons during a compositor grab are swallowed
	}
	if (pressed_buttons_ != 0 && held_.surface != nullptr) {
		// Every button goes where the first held one went, until all are up.
		pressed_buttons_ |= button_bit(button);
		wlr_seat_pointer_notify_button(server.seat, time, button,
			WL_POINTER_BUTTON_STATE_PRESSED);
		return;
	}
	Output *output = server.outputs.at(server.cursor->x, server.cursor->y);
	if (output == nullptr) {
		return;
	}
	double ox = server.cursor->x - output->lx, oy = server.cursor->y - output->ly;
	for (UiLayer *layer : input_.layers()) {
		if (layer->pointer_button(*output, ox, oy, button, true)) {
			return;
		}
	}
	if (!output->interactive()) {
		if (std::optional<LayerHit> shell = shell_surface_at(server, *output)) {
			focus_layer_on_click(server, *shell->layer, shell->surface);
			forward_press(time, button, shell->surface, shell->sx, shell->sy,
				surface_scale(*output, nullptr, shell->layer));
			return;
		}
		overview_press(*output, button);
		return;
	}

	bool super = input_.super_held();
	PointerHit hit = pointer_hit_test(server, *output);


	if (hit.view != nullptr && super && button == BTN_LEFT) {
		server.focus.focus_view(hit.view);
		input_.grab.begin_move(*hit.view, *output);
		return;
	}
	if (hit.view != nullptr && super && button == BTN_RIGHT) {
		server.focus.focus_view(hit.view);
		Point p = output->local_to_canvas(server.cursor->x - output->lx,
			server.cursor->y - output->ly);
		input_.grab.begin_resize(*hit.view, *output, quadrant_edges(*hit.view, p.x, p.y));
		return;
	}

	if (hit.surface != nullptr) {
		if (hit.view != nullptr) {
			server.focus.focus_view(hit.view);
		} else if (hit.layer != nullptr) {
			focus_layer_on_click(server, *hit.layer, hit.surface);
		}
		forward_press(time, button, hit.surface, hit.sx, hit.sy,
			surface_scale(*output, hit.view, hit.layer));
		return;
	}

	if (hit.view != nullptr) {
		// Somewhere on the server-side frame.
		server.focus.focus_view(hit.view);
		switch (hit.part) {
		case DecoPart::title:
			if (button == BTN_LEFT) {
				if (last_title_view == hit.view &&
						time - last_title_click_ <= config::double_click_ms) {
					last_title_view = nullptr;
					hit.view->set_maximized(hit.view->mode != WindowMode::maximized);
				} else {
					last_title_click_ = time;
					last_title_view = hit.view;
					input_.grab.begin_move(*hit.view, *output);
				}
			}
			return;
		case DecoPart::minimize:
		case DecoPart::maximize:
		case DecoPart::close:
			if (button == BTN_LEFT) {
				input_.grab.begin_title_button(*hit.view, *output, hit.part);
			}
			return;
		case DecoPart::resize:
			if (button == BTN_LEFT || button == BTN_RIGHT) {
				input_.grab.begin_resize(*hit.view, *output, hit.edges);
			}
			return;
		default:
			return;
		}
	}

	if (button == BTN_LEFT) {
		press_on_empty_canvas(server);
		// Panning needs Super; without it the press is a click
		// (docs/intents/canvas/infinite-canvas.md).
		if (super) {
			input_.grab.begin_pan(*output);
		}
	}
}

void PointerRouter::button_release(uint32_t time, uint32_t button) {
	if (server_.input.gate().locked()) {
		uint32_t bit = button_bit(button);
		if (pressed_buttons_ & bit) {
			pressed_buttons_ &= ~bit;
			wlr_seat_pointer_notify_button(server_.seat, time, button,
				WL_POINTER_BUTTON_STATE_RELEASED);
		}
		return;
	}
	// A release whose press reached a client reaches it too, also when the
	// client handed the drag that followed to the compositor.
	uint32_t bit = button_bit(button);
	bool forwarded = (pressed_buttons_ & bit) != 0;
	if (forwarded) {
		pressed_buttons_ &= ~bit;
		wlr_seat_pointer_notify_button(server_.seat, time, button,
			WL_POINTER_BUTTON_STATE_RELEASED);
	}
	bool hold_ends = forwarded && pressed_buttons_ == 0 && held_.surface != nullptr;
	if (hold_ends) {
		end_hold();
	}
	if (input_.grab.active()) {
		input_.grab.release();
		return;
	}
	if (forwarded) {
		if (hold_ends) {
			motion(time); // the pointer to what is under it now
		}
		return;
	}
	Output *output = server_.outputs.at(server_.cursor->x, server_.cursor->y);
	if (output == nullptr) {
		return;
	}
	double ox = server_.cursor->x - output->lx, oy = server_.cursor->y - output->ly;
	for (UiLayer *layer : input_.layers()) {
		if (layer->pointer_button(*output, ox, oy, button, false)) {
			return;
		}
	}
}

void PointerRouter::axis(const wlr_pointer_axis_event &event) {
	Server &server = server_;
	if (server.input.gate().locked()) {
		if (server.seat->pointer_state.focused_surface != nullptr) {
			wlr_seat_pointer_notify_axis(server.seat, event.time_msec,
				event.orientation, event.delta, event.delta_discrete,
				event.source, event.relative_direction);
		}
		return;
	}
	if (input_.grab.active()) {
		return;
	}
	Output *output = server.outputs.at(server.cursor->x, server.cursor->y);
	if (output == nullptr) {
		return;
	}
	double ox = server.cursor->x - output->lx, oy = server.cursor->y - output->ly;
	for (UiLayer *layer : input_.layers()) {
		if (layer->pointer_axis(*output, ox, oy, event.orientation, event.delta)) {
			return;
		}
	}
	// A shell surface under the pointer scrolls, in the overview too.
	if (output->overview.active() && !shell_surface_at(server, *output)) {
		if (input_.held_modifiers() & WLR_MODIFIER_CTRL) {
			output->overview.zoom_by(std::exp(-event.delta / config::overview_scroll_zoom_travel),
				server.cursor->x - output->lx, server.cursor->y - output->ly);
		} else {
			double delta = pan_delta(server, event);
			bool vertical = event.orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
			look_away(*output, vertical ? 0 : delta, vertical ? delta : 0);
		}
		return;
	}
	// Held Super decides a scroll in the normal view: the view looks away,
	// wherever the pointer is, and no client sees the scroll. The screen
	// moves by the scrolled distance whatever the zoom, the way the pan
	// switch says.
	if (!output->overview.active() && input_.super_held()) {
		if (event.delta != 0) {
			double delta = pan_delta(server, event);
			bool vertical = event.orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
			look_away(*output, vertical ? 0 : delta, vertical ? delta : 0);
		}
		return;
	}
	// Without Super a scroll over the empty canvas moves nothing.
	if (server.seat->pointer_state.focused_surface != nullptr) {
		wlr_seat_pointer_notify_axis(server.seat, event.time_msec,
			event.orientation, event.delta, event.delta_discrete,
			event.source, event.relative_direction);
	}
}

void PointerRouter::look_away(Output &output, double dx, double dy) {
	if (output.overview.active()) {
		Camera cam = output.camera.drawn();
		cam.cx += dx / cam.zoom;
		cam.cy += dy / cam.zoom;
		output.camera.jump(cam);
		return;
	}
	if (output.interactive()) {
		double zoom = output.camera.current().zoom;
		output.pan(dx / zoom, dy / zoom);
	}
}

std::optional<PointerRouter::ContactHit> PointerRouter::client_at(double lx, double ly) {
	Server &server = server_;
	Output *output = server.outputs.at(lx, ly);
	if (output == nullptr) {
		return std::nullopt;
	}
	double ox = lx - output->lx, oy = ly - output->ly;
	if (server.input.gate().locked()) {
		wlr_surface *surface = server.input.gate().surface_for(*output);
		if (surface == nullptr) {
			return std::nullopt;
		}
		return ContactHit{.surface = surface, .sx = ox, .sy = oy};
	}
	if (input_.layer_at(*output, ox, oy) != nullptr) {
		return std::nullopt;
	}
	if (!output->interactive()) {
		// The overview and a transition: shell surfaces only.
		std::optional<LayerHit> shell = shell_surface_at(*output, ox, oy);
		if (!shell.has_value()) {
			return std::nullopt;
		}
		return ContactHit{.surface = shell->surface, .sx = shell->sx, .sy = shell->sy,
			.scale = surface_scale(*output, nullptr, shell->layer),
			.layer = shell->layer};
	}
	PointerHit hit = pointer_hit_test(server, *output, ox, oy);
	if (hit.surface == nullptr) {
		return std::nullopt;
	}
	return ContactHit{.surface = hit.surface, .sx = hit.sx, .sy = hit.sy,
		.scale = surface_scale(*output, hit.view, hit.layer), .view = hit.view,
		.layer = hit.layer};
}

bool PointerRouter::canvas_at(double lx, double ly) {
	Server &server = server_;
	Output *output = server.outputs.at(lx, ly);
	if (output == nullptr || server.input.gate().locked()) {
		return false;
	}
	double ox = lx - output->lx, oy = ly - output->ly;
	if (input_.layer_at(*output, ox, oy) != nullptr
			|| shell_surface_at(*output, ox, oy).has_value()) {
		return false;
	}
	if (!output->interactive()) {
		return output->overview.active();
	}
	PointerHit hit = pointer_hit_test(server, *output, ox, oy);
	return hit.surface == nullptr && hit.view == nullptr;
}

void PointerRouter::contact_focus(const ContactHit &hit) {
	if (hit.view != nullptr) {
		server_.focus.focus_view(hit.view);
	} else if (hit.layer != nullptr) {
		focus_layer_on_click(server_, *hit.layer, hit.surface);
	}
}

void PointerRouter::contact_motion(uint32_t time, double lx, double ly) {
	wlr_cursor_warp_closest(server_.cursor, nullptr, lx, ly);
	motion(time);
}

void PointerRouter::contact_button(uint32_t time, uint32_t button, bool pressed) {
	if (pressed) {
		button_press(time, button);
	} else {
		button_release(time, button);
	}
	wlr_seat_pointer_notify_frame(server_.seat);
}
