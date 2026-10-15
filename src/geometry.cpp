#include <cmath>
#include <numbers>
#include <numeric>

#include "geometry.h"

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

std::vector<Rect> tile_layout(const Rect &area, std::size_t n, double gap) {
	if (n == 0) {
		return {};
	}
	std::vector<Rect> out;
	out.reserve(n);
	std::size_t cols = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
	std::size_t rows = (n + cols - 1) / cols;
	double cell_h = (area.h - gap * static_cast<double>(rows + 1)) / static_cast<double>(rows);
	std::size_t i = 0;
	for (std::size_t r = 0; r < rows; r++) {
		std::size_t in_row = r == rows - 1 ? n - cols * (rows - 1) : cols;
		double cell_w = (area.w - gap * static_cast<double>(in_row + 1)) / static_cast<double>(in_row);
		for (std::size_t c = 0; c < in_row; c++, i++) {
			out.push_back(Rect{
				.x = area.x + gap + static_cast<double>(c) * (cell_w + gap),
				.y = area.y + gap + static_cast<double>(r) * (cell_h + gap),
				.w = cell_w,
				.h = cell_h,
			});
		}
	}
	return out;
}

std::vector<std::size_t> tile_order(std::span<const Rect> frames, double band) {
	if (band <= 0) {
		band = 1;
	}
	std::vector<std::size_t> order(frames.size());
	std::iota(order.begin(), order.end(), std::size_t{0});
	// Insertion sort: n is a handful of windows.
	for (std::size_t i = 1; i < frames.size(); i++) {
		std::size_t cur = order[i];
		double cur_row = std::floor(frames[cur].cy() / band), cur_x = frames[cur].cx();
		std::size_t j = i;
		while (j > 0) {
			std::size_t prev = order[j - 1];
			double prev_row = std::floor(frames[prev].cy() / band);
			if (prev_row < cur_row || (prev_row == cur_row && frames[prev].cx() <= cur_x)) {
				break;
			}
			order[j] = prev;
			j--;
		}
		order[j] = cur;
	}
	return order;
}
