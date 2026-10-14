#ifndef CV_GEOMETRY_H
#define CV_GEOMETRY_H

#include <stdbool.h>
#include <stddef.h>

/* Pure canvas math: no wlroots types, unit-tested in tests/test_geometry.c. */

/* Canvas-space rectangle. */
struct rect {
	double x, y, w, h;
};

/* A view onto the canvas: the canvas point shown at the viewport centre and
 * the magnification (1 = one canvas unit per logical pixel). */
struct camera {
	double cx, cy, zoom;
};

static inline double rect_cx(struct rect r) {
	return r.x + r.w / 2;
}

static inline double rect_cy(struct rect r) {
	return r.y + r.h / 2;
}

double clampd(double v, double lo, double hi);
double ease_out_cubic(double t);

bool rect_contains(struct rect r, double x, double y);
bool rect_intersects(struct rect a, struct rect b);
double rect_overlap_area(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);

/* Canvas rectangle visible through `cam` on a vw x vh logical-pixel viewport. */
struct rect camera_viewport(struct camera cam, double vw, double vh);

/* Centre moves linearly, zoom geometrically; t in [0, 1]. Used for pans. */
struct camera camera_lerp(struct camera a, struct camera b, double t);

/*
 * Zoom transition between a close-up camera `near` (t = 0) and a zoomed-out
 * camera `far` (t = 1). Zoom changes geometrically while the region `near`
 * shows slides linearly on screen from the centre to where `far` displays it,
 * so zooming out visibly shrinks the current screen into its place on the
 * canvas (and zooming in grows the target out of it).
 */
struct camera camera_zoom_path(struct camera near, struct camera far, double t);

/* Largest zoom (at most max_zoom) that shows all of `content` inside a
 * vw x vh viewport with `margin` logical pixels free on every side. */
double fit_zoom(struct rect content, double vw, double vh, double margin, double max_zoom);

/*
 * Picks the candidate closest to (ox, oy) in direction (dx, dy): its centre
 * must lie within max_angle_deg of the direction; distance off-axis is
 * penalised so the most "straight ahead" neighbour wins over a slightly
 * nearer diagonal one. Index `exclude` is skipped (pass -1 for none).
 * Returns -1 if nothing lies in that direction.
 */
int nav_pick_direction(double ox, double oy, double dx, double dy,
	const struct rect *cands, size_t n, int exclude, double max_angle_deg);

/* Snaps a direction to one of the 8 compass directions (components -1/0/1). */
void nav_quantize_direction(double dx, double dy, int *qx, int *qy);

/* Grid of n cells filling `area`, `gap` between and around cells. Row-major;
 * the last row's cells widen to fill the row. */
void tile_layout(struct rect area, size_t n, double gap, struct rect *out);

/* Orders frames for tile_layout: rows (frame centres banded by `band` canvas
 * units) top to bottom, left to right within a row. Writes indices to order. */
void tile_order(const struct rect *frames, size_t n, double band, size_t *order);

#endif
