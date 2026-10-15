#include <cmath>
#include <optional>

#include "canvas/canvas.h"
#include "core/config.h"
#include "canvas/desktop.h"
#include "core/geometry.h"
#include "core/server.h"
#include "windows/view.h"

Desktop::Desktop(DesktopId id, int number, wlr_box frame)
	: id_(id), number_(number), frame_(frame) {
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
	return Point{
		.x = frame_.x + frame_.width / 2.0,
		.y = frame_.y + frame_.height / 2.0,
	};
}

bool Desktop::tiling() const {
	return tiling_;
}

bool Desktop::contains(double cx, double cy) const {
	return wlr_box_contains_point(&frame_, cx, cy);
}

Desktops::Desktops(Server &server)
	: server_(server) {
}

Desktop &Desktops::create(wlr_box frame) {
	int number = 1;
	while (by_number(number) != nullptr) {
		number++;
	}
	DesktopId id = next_id_;
	next_id_ = DesktopId{static_cast<int>(next_id_) + 1};
	stack_.insert(stack_.begin(), std::make_unique<Desktop>(id, number, frame));
	server_.damage_all();
	server_.ipc.emit_desktops();
	return *stack_.front();
}

void Desktops::remove(DesktopId id) {
	Desktop *desktop = find(id);
	if (desktop != nullptr && desktop->tiling()) {
		// Abandoning a tiled desktop leaves its windows where they are,
		// no longer tiled.
		for (View *view : server_.canvas.views_in(desktop->frame())) {
			view->set_tiled(false);
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

namespace {

// The frame centre of a view.
Point view_centre(const View &view) {
	wlr_box frame = view.frame_box();
	return Point{frame.x + frame.width / 2.0, frame.y + frame.height / 2.0};
}

} // namespace

void Desktops::move(DesktopId id, int x, int y) {
	Desktop *desktop = find(id);
	if (desktop == nullptr) {
		return;
	}
	if (desktop->tiling()) {
		// The one case a frame carries its windows: a tiled desktop keeps
		// its grid wherever the frame goes.
		int dx = x - desktop->frame_.x;
		int dy = y - desktop->frame_.y;
		for (View *view : server_.views) {
			if (!view->mapped || view->minimized) {
				continue;
			}
			Point centre = view_centre(*view);
			if (desktop->contains(centre.x, centre.y)) {
				view->move(view->x + dx, view->y + dy);
			}
		}
	}
	desktop->frame_.x = x;
	desktop->frame_.y = y;
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
