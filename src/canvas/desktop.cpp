#include <cmath>
#include <optional>

#include "core/config.h"
#include "canvas/desktop.h"
#include "core/geometry.h"
#include "core/server.h"

Desktop::Desktop(DesktopId id, int number, wlr_box frame)
	: id_(id), number_(number), base_w_(frame.width), base_h_(frame.height),
		frame_(frame) {
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

bool Desktop::contains(double cx, double cy) const {
	return wlr_box_contains_point(&frame_, cx, cy);
}

double Desktop::scale() const {
	return scale_;
}

void Desktop::set_scale(double scale) {
	// The frame fills the screen at the desktop's scale: the base size
	// (the output's logical size when the desktop was made) divided by
	// the scale, resized around the centre. Windows never move.
	double cx = frame_.x + frame_.width / 2.0;
	double cy = frame_.y + frame_.height / 2.0;
	int w = static_cast<int>(std::lround(base_w_ / scale));
	int h = static_cast<int>(std::lround(base_h_ / scale));
	frame_ = wlr_box{
		static_cast<int>(std::lround(cx - w / 2.0)),
		static_cast<int>(std::lround(cy - h / 2.0)),
		w,
		h,
	};
	scale_ = scale;
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
	return *stack_.front();
}

void Desktops::remove(DesktopId id) {
	destroy_owned(stack_, find(id));
	server_.damage_all();
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
	desktop->frame_.x = x;
	desktop->frame_.y = y;
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
