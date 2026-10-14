#include <math.h>
#include "geometry.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double clampd(double v, double lo, double hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

double ease_out_cubic(double t) {
	t = clampd(t, 0, 1) - 1;
	return t * t * t + 1;
}

bool rect_contains(struct rect r, double x, double y) {
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

bool rect_intersects(struct rect a, struct rect b) {
	return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

double rect_overlap_area(struct rect a, struct rect b) {
	double w = fmin(a.x + a.w, b.x + b.w) - fmax(a.x, b.x);
	double h = fmin(a.y + a.h, b.y + b.h) - fmax(a.y, b.y);
	return w > 0 && h > 0 ? w * h : 0;
}

struct rect rect_union(struct rect a, struct rect b) {
	double x0 = fmin(a.x, b.x), y0 = fmin(a.y, b.y);
	double x1 = fmax(a.x + a.w, b.x + b.w), y1 = fmax(a.y + a.h, b.y + b.h);
	return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

struct rect camera_viewport(struct camera cam, double vw, double vh) {
	double w = vw / cam.zoom, h = vh / cam.zoom;
	return (struct rect){cam.cx - w / 2, cam.cy - h / 2, w, h};
}

struct camera camera_lerp(struct camera a, struct camera b, double t) {
	return (struct camera){
		.cx = a.cx + (b.cx - a.cx) * t,
		.cy = a.cy + (b.cy - a.cy) * t,
		.zoom = exp(log(a.zoom) + (log(b.zoom) - log(a.zoom)) * t),
	};
}

struct camera camera_zoom_path(struct camera near, struct camera far, double t) {
	double zoom = exp(log(near.zoom) + (log(far.zoom) - log(near.zoom)) * t);
	// Screen offset of near's centre: 0 at t = 0, its place in `far` at t = 1.
	double sx = (near.cx - far.cx) * far.zoom * t;
	double sy = (near.cy - far.cy) * far.zoom * t;
	return (struct camera){
		.cx = near.cx - sx / zoom,
		.cy = near.cy - sy / zoom,
		.zoom = zoom,
	};
}

double fit_zoom(struct rect content, double vw, double vh, double margin, double max_zoom) {
	double aw = fmax(vw - 2 * margin, 1), ah = fmax(vh - 2 * margin, 1);
	double zoom = max_zoom;
	if (content.w > 0) {
		zoom = fmin(zoom, aw / content.w);
	}
	if (content.h > 0) {
		zoom = fmin(zoom, ah / content.h);
	}
	return zoom;
}

int nav_pick_direction(double ox, double oy, double dx, double dy,
		const struct rect *cands, size_t n, int exclude, double max_angle_deg) {
	double len = hypot(dx, dy);
	if (len == 0) {
		return -1;
	}
	double ux = dx / len, uy = dy / len;
	double max_angle = max_angle_deg * M_PI / 180;
	int best = -1;
	double best_score = INFINITY;
	for (size_t i = 0; i < n; i++) {
		if ((int)i == exclude) {
			continue;
		}
		double vx = rect_cx(cands[i]) - ox, vy = rect_cy(cands[i]) - oy;
		double along = vx * ux + vy * uy;
		double perp = fabs(vx * uy - vy * ux);
		if (along <= 1e-6 || atan2(perp, along) > max_angle) {
			continue;
		}
		double score = along + 2 * perp;
		if (score < best_score) {
			best_score = score;
			best = (int)i;
		}
	}
	return best;
}

void nav_quantize_direction(double dx, double dy, int *qx, int *qy) {
	*qx = *qy = 0;
	if (dx == 0 && dy == 0) {
		return;
	}
	// Sectors of 45 degrees centred on the compass directions.
	int sector = (int)lround(atan2(dy, dx) / (M_PI / 4));
	static const int dirs[8][2] = {
		{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
	};
	sector = ((sector % 8) + 8) % 8;
	*qx = dirs[sector][0];
	*qy = dirs[sector][1];
}

void tile_layout(struct rect area, size_t n, double gap, struct rect *out) {
	if (n == 0) {
		return;
	}
	size_t cols = (size_t)ceil(sqrt((double)n));
	size_t rows = (n + cols - 1) / cols;
	double cell_h = (area.h - gap * (double)(rows + 1)) / (double)rows;
	size_t i = 0;
	for (size_t r = 0; r < rows; r++) {
		size_t in_row = r == rows - 1 ? n - cols * (rows - 1) : cols;
		double cell_w = (area.w - gap * (double)(in_row + 1)) / (double)in_row;
		for (size_t c = 0; c < in_row; c++, i++) {
			out[i] = (struct rect){
				.x = area.x + gap + (double)c * (cell_w + gap),
				.y = area.y + gap + (double)r * (cell_h + gap),
				.w = cell_w,
				.h = cell_h,
			};
		}
	}
}

void tile_order(const struct rect *frames, size_t n, double band, size_t *order) {
	if (band <= 0) {
		band = 1;
	}
	for (size_t i = 0; i < n; i++) {
		order[i] = i;
	}
	// Insertion sort: n is a handful of windows.
	for (size_t i = 1; i < n; i++) {
		size_t cur = order[i];
		double cur_row = floor(rect_cy(frames[cur]) / band), cur_x = rect_cx(frames[cur]);
		size_t j = i;
		while (j > 0) {
			size_t prev = order[j - 1];
			double prev_row = floor(rect_cy(frames[prev]) / band);
			if (prev_row < cur_row || (prev_row == cur_row && rect_cx(frames[prev]) <= cur_x)) {
				break;
			}
			order[j] = prev;
			j--;
		}
		order[j] = cur;
	}
}
