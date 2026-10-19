#include <algorithm>
#include <cmath>
#include <optional>

#include "canvas/canvas.h"
#include "core/config.h"
#include "canvas/desktop.h"
#include "core/geometry.h"
#include "core/server.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

// A cell of the grid around desktop 1, in units of one desktop plus the gap.
struct Cell {
	int x, y;
};

// Ring `r`'s cells, in the fixed order: the four sides first, then the four
// corners, then the rest of the ring clockwise starting from the top. Ring 1
// is the slot order of desktops 2..9.
std::vector<Cell> ring_cells(int r) {
	std::vector<Cell> cells{
		{0, -r}, {r, 0}, {0, r}, {-r, 0},
		{r, -r}, {r, r}, {-r, r}, {-r, -r},
	};
	// The rest, clockwise from the top: each side's cells between its middle
	// and its corners.
	for (int x = 1; x < r; x++) {
		cells.push_back({x, -r}); // top row, right of the middle
	}
	for (int y = -r + 1; y < r; y++) {
		if (y != 0) {
			cells.push_back({r, y}); // right column, top to bottom
		}
	}
	for (int x = r - 1; x > -r; x--) {
		if (x != 0) {
			cells.push_back({x, r}); // bottom row, right to left
		}
	}
	for (int y = r - 1; y > -r; y--) {
		if (y != 0) {
			cells.push_back({-r, y}); // left column, bottom to top
		}
	}
	for (int x = -r + 1; x < 0; x++) {
		cells.push_back({x, -r}); // top row, left of the middle
	}
	return cells;
}

// The frame a desktop of `size` has at `cell` around `anchor`: a side cell
// sits directly beside the anchor's frame across the gap, centred along that
// side, and each further cell one frame and one gap on.
wlr_box cell_frame(wlr_box anchor, Cell cell, wlr_box size, int gap) {
	auto along = [gap](int start, int extent, int length, int step) {
		if (step == 0) {
			return start + (extent - length) / 2;
		}
		if (step > 0) {
			return start + extent + gap + (step - 1) * (length + gap);
		}
		return start + step * (length + gap);
	};
	return wlr_box{along(anchor.x, anchor.width, size.width, cell.x),
		along(anchor.y, anchor.height, size.height, cell.y), size.width, size.height};
}

} // namespace

Desktop::Desktop(DesktopId id, int number, wlr_box frame, double scale)
	: id_(id), number_(number), scale_(scale),
		base_w_(static_cast<int>(std::lround(frame.width * scale))),
		base_h_(static_cast<int>(std::lround(frame.height * scale))),
		frame_(frame),
		centre_{frame.x + frame.width / 2.0, frame.y + frame.height / 2.0} {
}

DesktopId Desktop::id() const {
	return id_;
}

int Desktop::number() const {
	return number_;
}

const wlr_box &Desktop::frame() const {
	return frame_;
}

Rect Desktop::rect() const {
	return Rect{
		static_cast<double>(frame_.x),
		static_cast<double>(frame_.y),
		static_cast<double>(frame_.width),
		static_cast<double>(frame_.height),
	};
}

Point Desktop::centre() const {
	return centre_;
}

bool Desktop::tiling() const {
	return tiling_;
}

bool Desktop::contains(double cx, double cy) const {
	return wlr_box_contains_point(&frame_, cx, cy);
}

double Desktop::scale() const {
	return scale_;
}

void Desktop::set_scale(double scale) {
	// The frame fills the screen at the desktop's scale: the base size (the
	// output's logical size when the desktop was made) divided by the
	// scale, around the exact centre, so back at 100 % it is the frame it
	// was. Windows never move.
	int w = static_cast<int>(std::lround(base_w_ / scale));
	int h = static_cast<int>(std::lround(base_h_ / scale));
	frame_ = wlr_box{
		static_cast<int>(std::lround(centre_.x - w / 2.0)),
		static_cast<int>(std::lround(centre_.y - h / 2.0)),
		w,
		h,
	};
	scale_ = scale;
}

Desktops::Desktops(Server &server)
	: server_(server) {
}

Desktop &Desktops::create(wlr_box frame, double scale) {
	return insert(smallest_unused_number(), frame, scale);
}

Desktop &Desktops::create_beside(int number, const Output &output) {
	// A desktop made this way starts at 100 %: the frame fills the screen.
	wlr_box size{0, 0, output.width, output.height};
	wlr_box anchor = anchor_frame();
	// Desktop 1's own place is the anchor cell; desktop N >= 2 wants slot
	// N - 1 of ring 1.
	if (number == 1) {
		wlr_box own = cell_frame(anchor, {0, 0}, size, config::desktop_gap);
		if (!overlaps_any(own)) {
			return insert(number, own, 1.0);
		}
	}
	std::size_t first = number >= 2 ? static_cast<std::size_t>(number - 2) : 0;
	return insert(number, free_frame_beside(anchor, size, first), 1.0);
}

Desktop &Desktops::create_next_to(wlr_box anchor, const Output &output) {
	wlr_box size{0, 0, output.width, output.height};
	return insert(smallest_unused_number(), free_frame_beside(anchor, size, 0), 1.0);
}

int Desktops::smallest_unused_number() const {
	int number = 1;
	while (std::ranges::any_of(stack_, [number](const std::unique_ptr<Desktop> &desktop) {
			return desktop->number() == number;
		})) {
		number++;
	}
	return number;
}

wlr_box Desktops::free_frame_beside(wlr_box anchor, wlr_box size, std::size_t first) const {
	auto free = [&](Cell cell) -> std::optional<wlr_box> {
		wlr_box candidate = cell_frame(anchor, cell, size, config::desktop_gap);
		if (overlaps_any(candidate)) {
			return std::nullopt;
		}
		return candidate;
	};
	// Ring 1 from slot `first`, wrapping round its start.
	std::vector<Cell> ring = ring_cells(1);
	for (std::size_t i = 0; i < ring.size(); i++) {
		if (std::optional<wlr_box> at = free(ring[(first + i) % ring.size()])) {
			return *at;
		}
	}
	// Every slot of ring 1 taken: the same order, one ring further out.
	for (int r = 2;; r++) {
		for (Cell cell : ring_cells(r)) {
			if (std::optional<wlr_box> at = free(cell)) {
				return *at;
			}
		}
	}
}

wlr_box Desktops::anchor_frame() const {
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		if (desktop->number() == 1) {
			return desktop->frame();
		}
	}
	// Desktop 1 abandoned: the first output's initial view, where desktop 1
	// is made at start-up.
	const Output &first = *server_.outputs.front();
	return wlr_box{first.lx, first.ly, first.width, first.height};
}

bool Desktops::overlaps_any(wlr_box box) const {
	Rect candidate{
		static_cast<double>(box.x),
		static_cast<double>(box.y),
		static_cast<double>(box.width),
		static_cast<double>(box.height),
	};
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		if (candidate.intersects(desktop->rect())) {
			return true;
		}
	}
	return false;
}

Desktop &Desktops::insert(int number, wlr_box frame, double scale) {
	DesktopId id = next_id_;
	next_id_ = DesktopId{static_cast<int>(next_id_) + 1};
	stack_.insert(stack_.begin(), std::make_unique<Desktop>(id, number, frame, scale));
	server_.damage_all();
	server_.ipc.emit_desktops();
	return *stack_.front();
}

void Desktops::remove(DesktopId id) {
	Desktop *desktop = find(id);
	if (desktop != nullptr && desktop->tiling()) {
		// Abandoning a tiled desktop leaves its windows where they are,
		// no longer tiled.
		for (View *view : server_.views) {
			if (server_.canvas.desktop_of(*view) == desktop) {
				view->set_tiled(false);
			}
		}
	}
	destroy_owned(stack_, desktop);
	server_.damage_all();
	server_.ipc.emit_desktops();
}

void Desktops::raise(DesktopId id) {
	auto it = std::ranges::find_if(stack_,
		[id](const std::unique_ptr<Desktop> &p) { return p->id() == id; });
	if (it == stack_.end()) {
		return;
	}
	std::unique_ptr<Desktop> owned = std::move(*it);
	stack_.erase(it);
	stack_.insert(stack_.begin(), std::move(owned));
}

void Desktops::move(DesktopId id, int x, int y) {
	Desktop *desktop = find(id);
	if (desktop == nullptr) {
		return;
	}
	int dx = x - desktop->frame_.x;
	int dy = y - desktop->frame_.y;
	if (desktop->tiling()) {
		// The one case a frame carries its windows: a tiled desktop keeps
		// its board wherever the frame goes.
		for (View *view : server_.views) {
			if (view->mapped && !view->minimized
					&& server_.canvas.desktop_of(*view) == desktop) {
				view->move(view->x + dx, view->y + dy);
			}
		}
	}
	desktop->frame_.x = x;
	desktop->frame_.y = y;
	desktop->centre_.x += dx;
	desktop->centre_.y += dy;
	// A moved frame takes windows in with it: their desktop may change.
	server_.visibility_dirty = true;
	server_.damage_all();
}

void Desktops::set_scale(DesktopId id, double scale) {
	Desktop *desktop = find(id);
	if (desktop == nullptr) {
		return;
	}
	desktop->set_scale(scale);
	server_.damage_all();
	server_.ipc.emit_desktops();
}

void Desktops::set_tiling(DesktopId id, bool on) {
	Desktop *desktop = find(id);
	if (desktop == nullptr || desktop->tiling_ == on) {
		return;
	}
	desktop->tiling_ = on;
	server_.damage_all(); // the tab's Tile switch lights up
}

Desktop *Desktops::find(DesktopId id) {
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		if (desktop->id() == id) {
			return desktop.get();
		}
	}
	return nullptr;
}

Desktop *Desktops::find(std::optional<DesktopId> id) {
	return id.has_value() ? find(*id) : nullptr;
}

const Desktop *Desktops::find(DesktopId id) const {
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		if (desktop->id() == id) {
			return desktop.get();
		}
	}
	return nullptr;
}

Desktop *Desktops::by_number(int number) {
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		if (desktop->number() == number) {
			return desktop.get();
		}
	}
	return nullptr;
}

Desktop *Desktops::at(double cx, double cy) {
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		if (desktop->contains(cx, cy)) {
			return desktop.get();
		}
	}
	return nullptr;
}

Desktop *Desktops::best_overlap(const Rect &area) {
	Desktop *best = nullptr;
	double best_overlap = area.w * area.h * 0.5;
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		double overlap = desktop->rect().overlap_area(area);
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

Desktop *Desktops::nearest(DesktopId from) {
	const Desktop *origin = find(from);
	if (origin == nullptr) {
		return nullptr;
	}
	Point centre = origin->centre();
	Desktop *best = nullptr;
	double best_dist = INFINITY;
	for (const std::unique_ptr<Desktop> &desktop : stack_) {
		if (desktop->id() == from) {
			continue;
		}
		Point other = desktop->centre();
		double dx = other.x - centre.x, dy = other.y - centre.y;
		double dist = dx * dx + dy * dy;
		if (dist < best_dist) {
			best_dist = dist;
			best = desktop.get();
		}
	}
	return best;
}

Desktop *Desktops::in_direction(double ox, double oy, double dx, double dy,
		std::optional<DesktopId> exclude) {
	if (stack_.empty()) {
		return nullptr;
	}
	std::vector<Rect> candidates;
	candidates.reserve(stack_.size());
	std::optional<std::size_t> skip;
	for (std::size_t i = 0; i < stack_.size(); i++) {
		candidates.push_back(stack_[i]->rect());
		if (exclude == stack_[i]->id()) {
			skip = i;
		}
	}
	std::optional<std::size_t> best = nav_pick_direction(ox, oy, dx, dy,
		candidates, skip, config::hop_cone_deg);
	return best.has_value() ? stack_[*best].get() : nullptr;
}

std::size_t Desktops::size() const {
	return stack_.size();
}

bool Desktops::empty() const {
	return stack_.empty();
}
