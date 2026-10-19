#include "platform/output.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <vector>

#include "canvas/canvas.h"
#include "core/config.h"
#include "canvas/desktop.h"
#include "core/server.h"
#include "windows/view.h"

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

// The board's slices as canvas boxes, edges rounded so neighbouring boxes
// stay a tile_gap apart at any rounding.
wlr_box slice_frame(const Rect &area, const Slice &slice) {
	Rect r = slice_box(area, slice, config::tile_gap);
	return wlr_box{
		static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y)),
		static_cast<int>(std::round(r.x + r.w)) - static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y + r.h)) - static_cast<int>(std::round(r.y)),
	};
}

// Reading order of where the views stand: rows top to bottom, left to right.
bool standing_before(const View *a, const View *b) {
	Rect ra = a->frame_rect(), rb = b->frame_rect();
	if (ra.cy() != rb.cy()) {
		return ra.cy() < rb.cy();
	}
	return ra.cx() < rb.cx();
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

} // namespace

Canvas::Canvas(Server &server)
	: desktops(server), fullscreen_desktops(server), server_(server) {
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
	// A window a tiled desktop's grid holds is that desktop's wherever a zoom
	// has moved the frame's edges; any other is the desktop its centre is in.
	if (Desktop *holder = desktops.find(view.board)) {
		return holder;
	}
	Point centre = box_centre(view.frame_box());
	return desktops.at(centre.x, centre.y);
}

std::vector<View *> Canvas::board_views(const Desktop &desktop) {
	std::vector<View *> out;
	for (View *view : server_.views) {
		if (out.size() == box_views_max) {
			break;
		}
		// Minimized windows keep their slice remembered but hold none of the
		// board while they are away.
		if (view->board == desktop.id() && view->mapped && !view->minimized) {
			out.push_back(view);
		}
	}
	return out;
}

std::vector<Slice> Canvas::held_slices(const Desktop &desktop) {
	std::vector<View *> views = board_views(desktop);
	std::vector<Slice> out;
	out.reserve(views.size());
	for (View *view : views) {
		out.push_back(*view->slice);
	}
	return out;
}

Rect Canvas::tiling_area(const Desktop &desktop) {
	Point centre = desktop.centre();
	const Output *picked = server_.output_under_cursor();
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		if (output->viewport().contains(centre.x, centre.y)) {
			picked = output.get();
			break;
		}
	}
	if (picked == nullptr) {
		return desktop.rect();
	}
	return box_to_rect(picked->usable_canvas_box(desktop.frame(), desktop.scale()));
}

void Canvas::tile(wlr_box area, const Output &output) {
	std::vector<View *> in_area = views_in(area);
	std::erase_if(in_area, [](const View *view) { return !tiles(view->kind()); });
	if (in_area.empty()) {
		server_.hint.set("No windows to tile here");
		return;
	}
	Rect usable = box_to_rect(output.usable_canvas_box(area, 1.0)); // a free spot rests at 1
	std::sort(in_area.begin(), in_area.end(), standing_before);
	std::vector<Slice> occupied;
	for (View *view : in_area) {
		std::optional<SlicePlacement> placed = place_new(occupied);
		if (!placed.has_value()) {
			continue;
		}
		if (placed->halved != no_slice) {
			// The holder keeps the left/top half of what was halved.
			View *holder = in_area[placed->halved];
			Slice kept = halve_slice(occupied[placed->halved]).first;
			holder->set_frame(slice_frame(usable, kept));
			occupied[placed->halved] = kept;
		}
		occupied.push_back(placed->slice);
		view->set_frame(slice_frame(usable, placed->slice));
	}
	server_.hint.set(std::format("Tiled {} window{}",
		in_area.size(), in_area.size() == 1 ? "" : "s"));
}

void Canvas::toggle_tiling(DesktopId id) {
	Desktop *desktop = desktops.find(id);
	if (desktop == nullptr) {
		return;
	}
	bool on = !desktop->tiling();
	desktops.set_tiling(id, on);
	if (on) {
		// Every window standing in the desktop joins the board, in reading
		// order of where it stands; maximized and fullscreen ones stay off it.
		std::vector<View *> standing;
		for (View *view : server_.views) {
			if (standing.size() == box_views_max) {
				break;
			}
			if (view->mapped && !view->minimized && !view->maximized
					&& !view->fullscreen && desktop->contains(
						view->frame_rect().cx(), view->frame_rect().cy())) {
				standing.push_back(view);
			}
		}
		std::sort(standing.begin(), standing.end(), standing_before);
		for (View *view : standing) {
			join(*desktop, *view);
		}
		server_.hint.set(std::format("Tiling on for Desktop {}",
			desktop->number()));
		return;
	}
	// Windows stay where the board left them, told they are no longer tiled;
	// the board and every remembered slice, minimized windows' included, is
	// dropped with it.
	for (View *view : server_.views) {
		if (view->board == id) {
			view->set_tiled(false);
			view->board = std::nullopt;
			view->slice = std::nullopt;
		}
	}
	server_.hint.set(std::format("Tiling off for Desktop {}",
		desktop->number()));
}

void Canvas::place_on_slice(View &view, const Desktop &desktop, Slice slice) {
	// A maximized or fullscreen view keeps its box: the slice is only its
	// reservation while the state lasts.
	if (view.maximized || view.fullscreen) {
		view.slice = slice;
		return;
	}
	view.slice = slice;
	view.board = desktop.id();
	view.set_frame(slice_frame(tiling_area(desktop), slice));
}

void Canvas::join(Desktop &desktop, View &view) {
	// Every way onto a board comes through here — a window opened, dropped,
	// combined in, tiling switched on — so the kind is asked here once.
	if (!tiles(view.kind())) {
		return;
	}
	std::vector<View *> views = board_views(desktop);
	std::vector<Slice> occupied = held_slices(desktop);
	std::optional<SlicePlacement> placed = place_new(occupied);
	if (!placed.has_value()) {
		return; // an empty board is all free; unreachable
	}
	if (placed->halved != no_slice) {
		View *holder = views[placed->halved];
		place_on_slice(*holder, desktop, halve_slice(*holder->slice).first);
	}
	place_on_slice(view, desktop, placed->slice);
}
void Canvas::step(View &view, int axis, int dir) {
	if (!view.slice.has_value() || !view.board.has_value()) {
		return;
	}
	Desktop *desktop = desktops.find(*view.board);
	if (desktop == nullptr || !desktop->tiling() || view.maximized
			|| view.fullscreen || view.minimized) {
		return;
	}
	Slice moved = step_slice(*view.slice, axis, dir);
	if (moved == *view.slice) {
		return; // already on that half; the near edge stays put
	}

	// The board after the move: the mover's new slice first, then every
	// window the growth does not cover.
	std::vector<View *> views{&view};
	std::vector<Slice> occupied{moved};
	std::vector<View *> covered;
	for (View *other : board_views(*desktop)) {
		if (other == &view) {
			continue;
		}
		if (other->slice->intersects(moved)) {
			covered.push_back(other);
		} else {
			views.push_back(other);
			occupied.push_back(*other->slice);
		}
	}
	std::sort(covered.begin(), covered.end(), standing_before);

	// The covered windows take their new slices in reading order; a push that
	// would halve the moved slice back means nowhere is left, and the whole
	// step is not taken.
	std::vector<std::pair<View *, Slice>> pushes;
	for (View *other : covered) {
		std::optional<SlicePlacement> placed = place_new(occupied, 0);
		if (!placed.has_value() || placed->halved == 0) {
			return;
		}
		if (placed->halved != no_slice) {
			// The window whose slice was halved keeps the left/top half.
			View *holder = views[placed->halved];
			occupied[placed->halved] = halve_slice(occupied[placed->halved]).first;
			pushes.emplace_back(holder, occupied[placed->halved]);
		}
		pushes.emplace_back(other, placed->slice);
		views.push_back(other);
		occupied.push_back(placed->slice);
	}
	place_on_slice(view, *desktop, moved);
	for (const auto &[pushed, slice] : pushes) {
		place_on_slice(*pushed, *desktop, slice);
	}
}

void Canvas::drop(View &view) {
	Point centre = box_centre(view.frame_box());
	if (view.board.has_value()) {
		// Dropped back on its own desktop: swap slices with the window
		// under the drop, or take the largest free slice under it.
		Desktop *home = desktops.find(*view.board);
		if (home != nullptr && home->tiling() && home->contains(centre.x, centre.y)) {
			Rect area = tiling_area(*home);
			Point at{(centre.x - area.x) / area.w, (centre.y - area.y) / area.h};
			for (View *other : board_views(*home)) {
				if (other != &view && other->slice->contains(at.x, at.y)) {
					std::swap(view.slice, other->slice);
					place_on_slice(view, *home, *view.slice);
					place_on_slice(*other, *home, *other->slice);
					return;
				}
			}
			if (std::optional<Slice> under = largest_free_slice(
					held_slices(*home), at)) {
				place_on_slice(view, *home, *under);
				return;
			}
			place_on_slice(view, *home, *view.slice); // nothing under: back to its slice
			return;
		}
		// Dropped outside its desktop: it leaves the board.
		leave_board(view);
		return;
	}
	// Not on a board: join whichever tiled desktop the drop landed in, or
	// stay a floating window where it was dropped.
	if (Desktop *onto = desktops.at(centre.x, centre.y); onto != nullptr
			&& onto->tiling() && !view.maximized && !view.fullscreen) {
		join(*onto, view);
	} else {
		view.set_tiled(false);
	}
}

void Canvas::leave_board(View &view) {
	view.set_tiled(false);
	view.board = std::nullopt;
	view.slice = std::nullopt;
}

void Canvas::settle(View &view) {
	// Opened, moved, minimized or a state toggled: the window returns to its
	// slice when it is still free, joins the board where it stands, or
	// leaves the board behind.
	if (view.board.has_value()) {
		Desktop *home = desktops.find(*view.board);
		bool holds = view.slice.has_value() && !view.minimized;
		if (home == nullptr || !home->tiling()) {
			leave_board(view);
		} else if (view.minimized) {
			// Away: the slice stays remembered but frees up for takers.
			if (view.tiled) {
				view.set_tiled(false);
			}
		} else if (!holds) {
			join(*home, view);
		} else if (view.maximized || view.fullscreen) {
			// Its slice stays reserved through the state.
			if (view.tiled) {
				view.set_tiled(false);
			}
		} else {
			Point centre = box_centre(view.frame_box());
			if (!home->contains(centre.x, centre.y)) {
				// Carried off the desktop by a move: the board lets it go.
				leave_board(view);
				settle(view);
				return;
			}
			// Back from minimize or a state change: its own slice when free,
			// else placed like a new one.
			std::vector<Slice> occupied = held_slices(*home);
			occupied.erase(std::remove_if(occupied.begin(), occupied.end(),
				[&view](const Slice &s) { return s == *view.slice; }),
				occupied.end());
			if (largest_free_slice(occupied) == *view.slice) {
				place_on_slice(view, *home, *view.slice);
			} else {
				join(*home, view);
			}
		}
		return;
	}
	// A window without a board joins the tiled desktop it stands on, if any.
	if (view.mapped && !view.minimized) {
		Point centre = box_centre(view.frame_box());
		if (Desktop *onto = desktops.at(centre.x, centre.y); onto != nullptr
				&& onto->tiling() && !view.maximized && !view.fullscreen) {
			join(*onto, view);
			return;
		}
	}
	if (view.tiled) {
		view.set_tiled(false);
	}
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
	std::vector<View *> incoming = views_in(from_area);
	for (View *view : incoming) {
		view->move(view->x + dx, view->y + dy);
		view->saved.x += dx;
		view->saved.y += dy;
		// Whatever board they came from is gone with their desktop.
		view->board = std::nullopt;
		view->slice = std::nullopt;
	}
	std::sort(incoming.begin(), incoming.end(), standing_before);
	int from_number = from_desktop->number(), into_number = into_desktop->number();
	bool from_tiled = from_desktop->tiling();
	desktops.remove(from);
	if (into_desktop->tiling()) {
		// The incoming windows join the board like new ones.
		for (View *view : incoming) {
			if (!view->maximized && !view->fullscreen) {
				join(*into_desktop, *view);
			}
		}
	} else if (from_tiled) {
		// A tiled desktop combined into an untiled one leaves its windows
		// where the translation put them, no longer tiled.
		for (View *view : incoming) {
			view->set_tiled(false);
		}
	}
	server_.hint.set(std::format("Combined Desktop {} into Desktop {}",
		from_number, into_number));
}

wlr_box Canvas::spot_at(double cx, double cy, const Output &output) {
	if (Desktop *desktop = desktops.at(cx, cy)) {
		return desktop->frame();
	}
	return centred_box(output, cx, cy);
}

void Canvas::settle_scale(DesktopId id, double scale) {
	Desktop *desktop = desktops.find(id);
	if (desktop == nullptr) {
		return;
	}
	desktops.set_scale(id, scale);
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		if (output->overview.active() && output->overview.target().desktop == id) {
			output->overview.set_target(desktop->frame(), id, false);
		}
	}
	// A settled scale is a completed action, named. It re-lays out nothing:
	// the windows keep their size and layout, a tiled grid its boxes.
	server_.hint.set(std::format("Desktop {} · {} %", desktop->number(),
		static_cast<int>(std::round(scale * 100))));
}
