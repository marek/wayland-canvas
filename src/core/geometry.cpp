#include <cmath>
#include <numbers>
#include <optional>

#include "core/geometry.h"

bool Rect::contains(double px, double py) const {
	return px >= x && px < x + w && py >= y && py < y + h;
}

bool Rect::intersects(const Rect &other) const {
	return x < other.x + other.w && other.x < x + w &&
		y < other.y + other.h && other.y < y + h;
}

double Rect::overlap_area(const Rect &other) const {
	double iw = std::fmin(x + w, other.x + other.w) - std::fmax(x, other.x);
	double ih = std::fmin(y + h, other.y + other.h) - std::fmax(y, other.y);
	return iw > 0 && ih > 0 ? iw * ih : 0;
}

Rect Rect::united(const Rect &other) const {
	double x0 = std::fmin(x, other.x), y0 = std::fmin(y, other.y);
	double x1 = std::fmax(x + w, other.x + other.w), y1 = std::fmax(y + h, other.y + other.h);
	return {x0, y0, x1 - x0, y1 - y0};
}

Rect Camera::viewport(double vw, double vh) const {
	double w = vw / zoom, h = vh / zoom;
	return {cx - w / 2, cy - h / 2, w, h};
}

double ease_out_cubic(double t) {
	t = clampd(t, 0, 1) - 1;
	return t * t * t + 1;
}

Camera camera_lerp(const Camera &a, const Camera &b, double t) {
	return {
		.cx = a.cx + (b.cx - a.cx) * t,
		.cy = a.cy + (b.cy - a.cy) * t,
		.zoom = std::exp(std::log(a.zoom) + (std::log(b.zoom) - std::log(a.zoom)) * t),
	};
}

Camera camera_zoom_path(const Camera &near, const Camera &far, double t) {
	double zoom = std::exp(std::log(near.zoom) + (std::log(far.zoom) - std::log(near.zoom)) * t);
	// Screen offset of near's centre: 0 at t = 0, its place in `far` at t = 1.
	double sx = (near.cx - far.cx) * far.zoom * t;
	double sy = (near.cy - far.cy) * far.zoom * t;
	return {
		.cx = near.cx - sx / zoom,
		.cy = near.cy - sy / zoom,
		.zoom = zoom,
	};
}

double fit_zoom(const Rect &content, double vw, double vh, double margin, double max_zoom) {
	double aw = std::fmax(vw - 2 * margin, 1), ah = std::fmax(vh - 2 * margin, 1);
	double zoom = max_zoom;
	if (content.w > 0) {
		zoom = std::fmin(zoom, aw / content.w);
	}
	if (content.h > 0) {
		zoom = std::fmin(zoom, ah / content.h);
	}
	return zoom;
}

int canvas_units_covered(int px, double zoom) {
	return static_cast<int>(std::ceil(px / zoom - 1e-9));
}

std::optional<std::size_t> nav_pick_direction(double ox, double oy, double dx, double dy,
	std::span<const Rect> candidates, std::optional<std::size_t> exclude,
	double max_angle_deg) {
	double len = std::hypot(dx, dy);
	if (len == 0) {
		return std::nullopt;
	}
	double ux = dx / len, uy = dy / len;
	double max_angle = max_angle_deg * std::numbers::pi / 180;
	std::optional<std::size_t> best;
	double best_score = HUGE_VAL;
	for (std::size_t i = 0; i < candidates.size(); i++) {
		if (exclude == i) {
			continue;
		}
		double vx = candidates[i].cx() - ox, vy = candidates[i].cy() - oy;
		double along = vx * ux + vy * uy;
		double perp = std::fabs(vx * uy - vy * ux);
		if (along <= 1e-6 || std::atan2(perp, along) > max_angle) {
			continue;
		}
		double score = along + 2 * perp;
		if (score < best_score) {
			best_score = score;
			best = i;
		}
	}
	return best;
}

Compass nav_quantize_direction(double dx, double dy) {
	if (dx == 0 && dy == 0) {
		return {};
	}
	// Sectors of 45 degrees centred on the compass directions.
	int sector = static_cast<int>(std::lround(std::atan2(dy, dx) / (std::numbers::pi / 4)));
	constexpr int dirs[8][2] = {
		{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
	};
	sector = ((sector % 8) + 8) % 8;
	return {dirs[sector][0], dirs[sector][1]};
}

bool Slice::contains(double px, double py) const {
	return x0 <= px && px < x1 && y0 <= py && py < y1;
}

bool Slice::intersects(const Slice &other) const {
	return x0 < other.x1 && other.x0 < x1 && y0 < other.y1 && other.y0 < y1;
}

// The fractions are dyadic, so the half tests are exact.
int Slice::span_x() const {
	if (x0 <= 0 && x1 >= 1) {
		return 1;
	}
	return x1 <= 0.5 ? 0 : 2;
}

int Slice::span_y() const {
	if (y0 <= 0 && y1 >= 1) {
		return 1;
	}
	return y1 <= 0.5 ? 0 : 2;
}

namespace {

// The half of the board a span steps to: toward the first half (0) the far
// half widens to the whole and the whole snaps to the near half; toward the
// second half (2), the other way around; the near half never moves.
double stepped_edge(int span, int dir, bool high) {
	int to = span;
	if (dir < 0) {
		to = span == 2 ? 1 : 0;
	} else if (dir > 0) {
		to = span == 0 ? 1 : 2;
	}
	if (!high) {
		return to == 0 ? 0 : (to == 1 ? 0 : 0.5);
	}
	return to == 0 ? 0.5 : (to == 1 ? 1 : 1);
}

} // namespace

Slice step_slice(Slice slice, int axis, int dir) {
	Slice out = slice;
	if (axis == 0) {
		out.x0 = stepped_edge(slice.span_x(), dir, false);
		out.x1 = stepped_edge(slice.span_x(), dir, true);
	} else {
		out.y0 = stepped_edge(slice.span_y(), dir, false);
		out.y1 = stepped_edge(slice.span_y(), dir, true);
	}
	return out;
}

bool reading_before(const Slice &a, const Slice &b) {
	if (a.y0 != b.y0) {
		return a.y0 < b.y0;
	}
	if (a.x0 != b.x0) {
		return a.x0 < b.x0;
	}
	// Same top-left corner: the slice that ends first is read first.
	if (a.y1 != b.y1) {
		return a.y1 < b.y1;
	}
	return a.x1 < b.x1;
}

std::pair<Slice, Slice> halve_slice(Slice slice) {
	if (slice.x1 - slice.x0 >= slice.y1 - slice.y0) {
		double mid = (slice.x0 + slice.x1) / 2;
		return {{slice.x0, slice.y0, mid, slice.y1}, {mid, slice.y0, slice.x1, slice.y1}};
	}
	double mid = (slice.y0 + slice.y1) / 2;
	return {{slice.x0, slice.y0, slice.x1, mid}, {slice.x0, mid, slice.x1, slice.y1}};
}

namespace {

// Depth at which to stop looking for free slices: past it a slice is smaller
// than any a window count under the board's cap can ever produce.
constexpr int slice_max_depth = 12;

// The whole board's four halves; any deeper slice only splits along its
// longer side, because only steps make the cross-aligned halves.
void child_slices(const Slice &s, Slice out[4], std::size_t &count) {
	if (s.x0 <= 0 && s.x1 >= 1 && s.y0 <= 0 && s.y1 >= 1) {
		out[count++] = {0, 0, 0.5, 1};
		out[count++] = {0.5, 0, 1, 1};
		out[count++] = {0, 0, 1, 0.5};
		out[count++] = {0, 0.5, 1, 1};
		return;
	}
	auto [low, high] = halve_slice(s);
	out[count++] = low;
	out[count++] = high;
}

} // namespace

std::optional<Slice> largest_free_slice(std::span<const Slice> occupied,
		std::optional<Point> under) {
	// Every level of the slice tree halves every slice, so all candidates at
	// one depth cover the same area: breadth-first finds the largest first,
	// and reading order settles ties within a depth.
	std::optional<std::pair<Slice, int>> best;
	std::vector<std::pair<Slice, int>> queue{{Slice{0, 0, 1, 1}, 0}};
	for (std::size_t i = 0; i < queue.size(); i++) {
		Slice s = queue[i].first;
		int depth = queue[i].second;
		if (under.has_value()
				&& !(s.x0 <= under->x && under->x < s.x1 && s.y0 <= under->y
					&& under->y < s.y1)) {
			continue;
		}
		bool covered = false;
		bool inside = false;
		for (const Slice &occ : occupied) {
			if (occ.intersects(s)) {
				covered = true;
			}
			inside = inside || (s.x0 >= occ.x0 && s.x1 <= occ.x1 && s.y0 >= occ.y0
				&& s.y1 <= occ.y1);
		}
		if (!covered) {
			if (!best.has_value() || depth < best->second
					|| (depth == best->second && reading_before(s, best->first))) {
				best = {s, depth};
			}
			continue;
		}
		if (inside || depth >= slice_max_depth) {
			continue; // no room inside this slice; stop splitting it
		}
		Slice children[4];
		std::size_t count = 0;
		child_slices(s, children, count);
		for (std::size_t k = 0; k < count; k++) {
			queue.emplace_back(children[k], depth + 1);
		}
	}
	if (!best.has_value()) {
		return std::nullopt;
	}
	return best->first;
}

std::optional<SlicePlacement> place_new(std::span<const Slice> occupied,
		std::optional<std::size_t> exclude) {
	if (std::optional<Slice> free = largest_free_slice(occupied)) {
		return SlicePlacement{*free, no_slice};
	}
	// Nothing free: halve the largest slice other than the excluded one, ties
	// in reading order.
	auto area = [](const Slice &s) {
		return (s.x1 - s.x0) * (s.y1 - s.y0);
	};
	std::optional<std::size_t> target;
	for (std::size_t i = 0; i < occupied.size(); i++) {
		if (exclude == i) {
			continue;
		}
		if (!target.has_value() || area(occupied[i]) > area(occupied[*target])
				|| (area(occupied[i]) == area(occupied[*target])
					&& reading_before(occupied[i], occupied[*target]))) {
			target = i;
		}
	}
	// Every slice but the moved one is covered by it: the push has nowhere to
	// go, so it halves the moved slice back instead of leaving a window homeless.
	if (!target.has_value()) {
		target = exclude;
	}
	if (!target.has_value()) {
		return std::nullopt; // an empty board is all free; cannot happen
	}
	auto [kept, freed] = halve_slice(occupied[*target]);
	return SlicePlacement{freed, *target};
}

Rect slice_box(const Rect &area, const Slice &slice, double gap) {
	Rect out{
		area.x + slice.x0 * area.w + gap / 2,
		area.y + slice.y0 * area.h + gap / 2,
		(slice.x1 - slice.x0) * area.w - gap,
		(slice.y1 - slice.y0) * area.h - gap,
	};
	if (out.w < 1) {
		out.w = 1;
	}
	if (out.h < 1) {
		out.h = 1;
	}
	return out;
}
