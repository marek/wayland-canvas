// grab.cpp: the compositor-owned pointer grabs: window moves and resizes,
// canvas pans, title bar buttons, overview presses, desktop frame drags and
// the drags of plugins' surfaces on the plane.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>

#include "core/config.h"
#include "input/grab.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "core/server.h"
#include "windows/view.h"

namespace {

template <typename... Ts>
struct overloaded : Ts... {
	using Ts::operator()...;
};
template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// The view an overview press landed on, when it landed on one.
View *press_view(const PressTarget &target) {
	if (const auto *on_view = std::get_if<PressOnView>(&target)) {
		return on_view->view;
	}
	if (const auto *on_shelf = std::get_if<PressOnShelf>(&target)) {
		return on_shelf->view;
	}
	return nullptr;
}

// The output a grab state began on; None holds none.
Output *grab_output(const PointerGrab::State &state) {
	return std::visit(overloaded{
		[](const PointerGrab::None &) -> Output * { return nullptr; },
		[](const auto &s) -> Output * { return s.output; },
	}, state);
}

// Target the desktop a view belongs to, or a screen-sized free spot
// centred on the view, and leave the overview onto it.
void overview_target_view(Server &server, Output &output, View &view) {
	Desktop *desktop = server.canvas.desktop_of(view);
	if (desktop != nullptr) {
		output.overview.set_target(desktop->frame(), desktop->id(), true);
	} else {
		wlr_box frame = view.frame_box();
		wlr_box box{
			.x = frame.x + frame.width / 2 - output.width / 2,
			.y = frame.y + frame.height / 2 - output.height / 2,
			.width = output.width,
			.height = output.height,
		};
		output.overview.set_target(box, std::nullopt, true);
	}
}

} // namespace

PointerGrab::PointerGrab(Server &server)
	: server_(server) {
}

bool PointerGrab::active() const {
	return !std::holds_alternative<None>(state_);
}

const PointerGrab::State &PointerGrab::state() const {
	return state_;
}

void PointerGrab::begin_move(View &view, Output &output) {
	Point p = output.local_to_canvas(server_.cursor->x - output.lx,
		server_.cursor->y - output.ly);
	Move move{
		.view = &view,
		.output = &output,
		.cam = output.camera.drawn(),
		.lx = server_.cursor->x,
		.ly = server_.cursor->y,
		.start = view.content_box(),
		.moved = false,
	};
	if (view.maximized) {
		// Remember where on the title bar the grab started so the
		// drag-restore below can keep the cursor at the same spot.
		wlr_box frame = view.frame_box();
		move.rel_x = frame.width > 0
			? clampd((p.x - frame.x) / frame.width, 0.0, 1.0) : 0.5;
		move.titlebar_offset = clampd(p.y - frame.y, 0.0,
			double(view.deco.titlebar()));
	}
	state_ = move;
}

void PointerGrab::begin_resize(View &view, Output &output, uint32_t edges) {
	if (edges == 0) {
		edges = WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
	}
	wlr_box start = view.content_box();
	state_ = Resize{
		.view = &view,
		.output = &output,
		.cam = output.camera.drawn(),
		.lx = server_.cursor->x,
		.ly = server_.cursor->y,
		.start = start,
		.edges = edges,
	};
	// view.cpp anchors the moving (left/top) edges on commit while resizing.
	view.resize_edges = edges;
	view.resize_end_serial = 0;
	view.resize_start = start;
	if (view.wlr != nullptr) {
		wlr_xdg_toplevel_set_resizing(view.wlr, true);
	}
	server_.input.set_cursor_image(
		wlr_xcursor_get_resize_name(static_cast<wlr_edges>(edges)));
}

void PointerGrab::begin_pan(Output &output) {
	state_ = Pan{
		.output = &output,
		.cam = output.camera.drawn(),
		.lx = server_.cursor->x,
		.ly = server_.cursor->y,
	};
}

void PointerGrab::begin_title_button(View &view, Output &output,
		DecoPart part) {
	state_ = TitleButton{.view = &view, .output = &output, .part = part};
	view.deco.pressed = part;
	view.damage();
}

void PointerGrab::begin_overview_press(Output &output, PressTarget target) {
	state_ = OverviewPress{
		.output = &output,
		.cam = output.camera.drawn(),
		.lx = server_.cursor->x,
		.ly = server_.cursor->y,
		.target = target,
	};
}

void PointerGrab::begin_surface_drag(LayerSurface &layer, Output &output,
		double lx, double ly, uint32_t edges, int min_w, int min_h) {
	SurfaceDrag drag{
		.layer = &layer,
		.output = &output,
		.cam = output.camera.drawn(),
		.lx = lx,
		.ly = ly,
		.start = layer.canvas_box.value_or(wlr_box{}),
		.edges = edges,
		.min_w = std::max(min_w, 1),
		.min_h = std::max(min_h, 1),
	};
	state_ = drag;
	if (edges != 0) {
		server_.input.set_cursor_image(
			wlr_xcursor_get_resize_name(static_cast<wlr_edges>(edges)));
	}
	// The pointer may have travelled since the press: the surface catches up.
	surface_drag_motion(std::get<SurfaceDrag>(state_));
}


void PointerGrab::motion() {
	std::visit(overloaded{
		[this](Move &s) { move_motion(s); },
		[this](Resize &s) { resize_motion(s); },
		[this](Pan &s) { pan_motion(s); },
		[this](TitleButton &s) { title_button_motion(s); },
		[this](OverviewPress &s) { overview_press_motion(s); },
		[this](DesktopDrag &s) { desktop_drag_motion(s); },
		[this](SurfaceDrag &s) { surface_drag_motion(s); },
		[](None &) {},
	}, state_);
}

void PointerGrab::move_motion(Move &move) {
	View *view = move.view;
	Output *output = move.output;
	double dist = std::hypot(server_.cursor->x - move.lx,
		server_.cursor->y - move.ly);
	if (view->maximized) {
		if (dist < config::drag_threshold) {
			return;
		}
		// Restore under the cursor: same relative x across the title bar,
		// same height into it.
		view->set_maximized(false);
		wlr_box restored = view->saved;
		if (restored.width <= 0 || restored.height <= 0) {
			restored = view->content_box();
		}
		int border = view->deco.border();
		int title = view->deco.titlebar();
		Point p = output->local_to_canvas(server_.cursor->x - output->lx,
			server_.cursor->y - output->ly);
		double frame_w = restored.width + 2.0 * border;
		int frame_left = static_cast<int>(std::round(p.x - move.rel_x * frame_w));
		int frame_top = static_cast<int>(std::round(p.y - move.titlebar_offset));
		view->move(frame_left + border, frame_top + title + border);
		move.start.x = view->x;
		move.start.y = view->y;
		move.start.width = restored.width;
		move.start.height = restored.height;
		move.lx = server_.cursor->x;
		move.ly = server_.cursor->y;
		move.moved = true;
		return;
	}
	if (dist >= config::drag_threshold) {
		move.moved = true;
	}
	double dx = (server_.cursor->x - move.lx) / move.cam.zoom;
	double dy = (server_.cursor->y - move.ly) / move.cam.zoom;
	view->move(static_cast<int>(std::round(move.start.x + dx)),
		static_cast<int>(std::round(move.start.y + dy)));
}

void PointerGrab::resize_motion(Resize &resize) {
	View *view = resize.view;
	if (view->wlr == nullptr) {
		return;
	}
	double dx = (server_.cursor->x - resize.lx) / resize.cam.zoom;
	double dy = (server_.cursor->y - resize.ly) / resize.cam.zoom;
	double x1 = resize.start.x;
	double y1 = resize.start.y;
	double x2 = x1 + resize.start.width;
	double y2 = y1 + resize.start.height;
	if (resize.edges & WLR_EDGE_LEFT) {
		x1 += dx;
	}
	if (resize.edges & WLR_EDGE_RIGHT) {
		x2 += dx;
	}
	if (resize.edges & WLR_EDGE_TOP) {
		y1 += dy;
	}
	if (resize.edges & WLR_EDGE_BOTTOM) {
		y2 += dy;
	}

	wlr_xdg_toplevel *toplevel = view->wlr;
	double min_w = 100, min_h = 60;
	if (toplevel->current.min_width > min_w) {
		min_w = toplevel->current.min_width;
	}
	if (toplevel->current.min_height > min_h) {
		min_h = toplevel->current.min_height;
	}
	double max_w = toplevel->current.max_width > 0
		? toplevel->current.max_width : 1e9;
	double max_h = toplevel->current.max_height > 0
		? toplevel->current.max_height : 1e9;
	double w = clampd(x2 - x1, min_w, max_w);
	double h = clampd(y2 - y1, min_h, max_h);
	wlr_xdg_toplevel_set_size(toplevel, static_cast<int32_t>(std::round(w)),
		static_cast<int32_t>(std::round(h)));
}

void PointerGrab::pan_motion(Pan &pan) {
	// Content follows the cursor.
	Camera cam = pan.cam;
	cam.cx -= (server_.cursor->x - pan.lx) / cam.zoom;
	cam.cy -= (server_.cursor->y - pan.ly) / cam.zoom;
	if (pan.output->overview.active()) {
		pan.output->camera.jump(cam);
		return;
	}
	pan.output->look_to(cam); // the normal view's pan is a look away
}

void PointerGrab::title_button_motion(TitleButton &button) {
	View *view = button.view;
	std::optional<CanvasPos> pos = server_.input.cursor_canvas_pos();
	if (!pos.has_value()) {
		return;
	}
	DecoHit hit = view->deco.hit(pos->cx, pos->cy,
		pos->output->camera.drawn().zoom);
	DecoPart pressed = hit.part == button.part ? button.part : DecoPart::none;
	if (view->deco.pressed != pressed) {
		view->deco.pressed = pressed;
		view->damage();
	}
}

void PointerGrab::overview_press_motion(OverviewPress &press) {
	double dist = std::hypot(server_.cursor->x - press.lx,
		server_.cursor->y - press.ly);
	if (dist < config::drag_threshold) {
		return;
	}
	Output *output = press.output;
	Camera cam = press.cam;
	std::visit(overloaded{
		[&](PressOnDesktop &on) {
			if (Desktop *desktop = server_.canvas.desktops.find(on.desktop)) {
				state_ = DesktopDrag{
					.output = output,
					.desktop = desktop->id(),
					.cam = cam,
					.lx = server_.cursor->x,
					.ly = server_.cursor->y,
					.start = desktop->frame(),
					.drop = std::nullopt,
				};
			}
		},
		[&](PressOnView &on) {
			if (on.view != nullptr) {
				begin_move(*on.view, *output);
			}
		},
		[&](PressOnEmpty &) {
			begin_pan(*output);
		},
		// Tab buttons and shelf items stay clicks.
		[](const auto &) {},
	}, press.target);
}

void PointerGrab::desktop_drag_motion(DesktopDrag &drag) {
	Desktop *desktop = server_.canvas.desktops.find(drag.desktop);
	if (desktop == nullptr) {
		// Its desktop was removed mid-drag; nothing follows the cursor.
		return;
	}
	double dx = (server_.cursor->x - drag.lx) / drag.cam.zoom;
	double dy = (server_.cursor->y - drag.ly) / drag.cam.zoom;
	server_.canvas.desktops.move(desktop->id(),
		static_cast<int>(std::round(drag.start.x + dx)),
		static_cast<int>(std::round(drag.start.y + dy)));

	// Combine when another desktop is overlapped by at least half of the
	// dragged frame; largest overlap wins.
	Rect box = desktop->rect();
	double area = desktop->frame().width * double(desktop->frame().height);
	std::optional<DesktopId> drop;
	double best = 0;
	for (const Desktop &d : server_.canvas.desktops.all()) {
		if (d.id() == desktop->id()) {
			continue;
		}
		double overlap = box.overlap_area(d.rect());
		if (overlap >= config::combine_overlap * area && overlap > best) {
			best = overlap;
			drop = d.id();
		}
	}
	drag.drop = drop;
}

void PointerGrab::surface_drag_motion(SurfaceDrag &drag) {
	double dx = (server_.cursor->x - drag.lx) / drag.cam.zoom;
	double dy = (server_.cursor->y - drag.ly) / drag.cam.zoom;
	double x1 = drag.start.x, y1 = drag.start.y;
	double x2 = x1 + drag.start.width, y2 = y1 + drag.start.height;
	if (drag.edges == 0) {
		x1 += dx;
		x2 += dx;
		y1 += dy;
		y2 += dy;
	}
	// A sized edge stops where the minimum would be crossed; the opposite
	// edge never moves.
	if (drag.edges & WLR_EDGE_LEFT) {
		x1 = std::fmin(x1 + dx, x2 - drag.min_w);
	}
	if (drag.edges & WLR_EDGE_RIGHT) {
		x2 = std::fmax(x2 + dx, x1 + drag.min_w);
	}
	if (drag.edges & WLR_EDGE_TOP) {
		y1 = std::fmin(y1 + dy, y2 - drag.min_h);
	}
	if (drag.edges & WLR_EDGE_BOTTOM) {
		y2 = std::fmax(y2 + dy, y1 + drag.min_h);
	}
	int left = static_cast<int>(std::round(x1));
	int top = static_cast<int>(std::round(y1));
	wlr_box box{left, top, static_cast<int>(std::round(x2)) - left,
		static_cast<int>(std::round(y2)) - top};
	if (drag.layer->canvas_box.has_value() && wlr_box_equal(&*drag.layer->canvas_box, &box)) {
		return;
	}
	drag.layer->place(box);
}

void PointerGrab::release() {
	std::visit(overloaded{
		[this](TitleButton &s) { release_title_button(s); },
		[this](OverviewPress &s) { overview_click(s); },
		[this](DesktopDrag &s) { drop_desktop_drag(s); },
		// A user move ends on the board: swap slices with the window under
		// the drop, take the largest free slice under it, or leave the
		// board when dropped outside. A resize ends back on its slice.
	[this](Move &s) { server_.canvas.drop(*s.view); },
	[this](Resize &s) { server_.canvas.settle(*s.view); },
		[this](SurfaceDrag &s) { server_.ipc.answer_surface_drag(s.layer->canvas_box); },
		[](auto &) {},
	}, state_);
	cancel();
}

void PointerGrab::release_title_button(TitleButton &button) {
	View *view = button.view;
	std::optional<CanvasPos> pos = server_.input.cursor_canvas_pos();
	if (pos.has_value()) {
		DecoHit hit = view->deco.hit(pos->cx, pos->cy,
			pos->output->camera.drawn().zoom);
		if (hit.part == button.part) {
			switch (hit.part) {
			case DecoPart::minimize:
				view->set_minimized(true);
				break;
			case DecoPart::maximize:
				view->set_maximized(!view->maximized);
				break;
			case DecoPart::close:
				view->close();
				break;
			default:
				break;
			}
		}
	}
	if (view->deco.pressed != DecoPart::none) {
		view->deco.pressed = DecoPart::none;
		view->damage();
	}
}

void PointerGrab::overview_click(OverviewPress &press) {
	Output *output = press.output;
	std::visit(overloaded{
		[&](PressOnShelf &on) {
			if (on.view == nullptr) {
				return;
			}
			on.view->set_minimized(false);
			server_.focus_view(on.view);
			overview_target_view(server_, *output, *on.view);
			output->overview.exit();
		},
		[&](PressOnTile &on) {
			if (Desktop *desktop = server_.canvas.desktops.find(on.desktop)) {
				output->overview.tile(desktop->id());
			}
		},
		[&](PressOnAbandon &on) {
			if (Desktop *desktop = server_.canvas.desktops.find(on.desktop)) {
				output->overview.abandon(desktop->id());
			}
		},
		[&](PressOnDesktop &on) {
			if (Desktop *desktop = server_.canvas.desktops.find(on.desktop)) {
				output->overview.set_target(desktop->frame(), desktop->id(), true);
				output->overview.exit();
			}
		},
		[&](PressOnView &on) {
			if (on.view == nullptr) {
				return;
			}
			server_.focus_view(on.view);
			overview_target_view(server_, *output, *on.view);
			output->overview.exit();
		},
		[&](PressOnEmpty &) {
			if (std::optional<CanvasPos> pos =
					server_.input.cursor_canvas_pos()) {
				output->overview.target_point(pos->cx, pos->cy);
				output->overview.exit();
			}
		},
	}, press.target);
}

void PointerGrab::drop_desktop_drag(DesktopDrag &drag) {
	Desktop *drop = server_.canvas.desktops.find(drag.drop);
	Desktop *grabbed = server_.canvas.desktops.find(drag.desktop);
	if (drop != nullptr && grabbed != nullptr) {
		server_.canvas.combine(drop->id(), grabbed->id(), drag.start);
	} else if (grabbed != nullptr) {
		server_.canvas.desktops.raise(grabbed->id());
	}
}


void PointerGrab::cancel() {
	if (const auto *move = std::get_if<Move>(&state_)) {
		end_view_grab(*move->view);
	} else if (const auto *resize = std::get_if<Resize>(&state_)) {
		end_view_grab(*resize->view);
	} else if (const auto *button = std::get_if<TitleButton>(&state_)) {
		end_view_grab(*button->view);
	} else if (std::holds_alternative<SurfaceDrag>(state_)) {
		// Not ended by its release (the surface or the output went): the
		// client still waiting is told, a release already answered it.
		server_.ipc.answer_surface_drag(std::nullopt);
	}
	state_ = None{};
	server_.damage_all();
	server_.input.set_cursor_image("default");
}

void PointerGrab::end_view_grab(View &view) {
	view.deco.pressed = DecoPart::none;
	if (view.resize_edges != 0 && view.resize_end_serial == 0) {
		// view.cpp keeps anchoring until the client acks this configure.
		view.resize_end_serial =
			wlr_xdg_toplevel_set_resizing(view.wlr, false);
	}
	view.damage();
}

bool PointerGrab::panning(const Output &output) const {
	const Pan *pan = std::get_if<Pan>(&state_);
	return pan != nullptr && pan->output == &output;
}

std::optional<DesktopId> PointerGrab::drop_target() const {
	if (const DesktopDrag *drag = std::get_if<DesktopDrag>(&state_)) {
		return drag->drop;
	}
	return std::nullopt;
}

void PointerGrab::forget(const View &view) {
	if (std::visit(overloaded{
		[&view](const Move &s) { return s.view == &view; },
		[&view](const Resize &s) { return s.view == &view; },
		[&view](const TitleButton &s) { return s.view == &view; },
		[&view](const OverviewPress &s) { return press_view(s.target) == &view; },
		[](const auto &) { return false; },
	}, state_)) {
		cancel();
	}
}

void PointerGrab::forget(const Output &output) {
	if (grab_output(state_) == &output) {
		cancel();
	}
}

void PointerGrab::forget(const LayerSurface &layer) {
	const SurfaceDrag *drag = std::get_if<SurfaceDrag>(&state_);
	if (drag != nullptr && drag->layer == &layer) {
		cancel();
	}
}
