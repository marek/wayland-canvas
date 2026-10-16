#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// Pure canvas math: no wlroots types, unit-tested in tests/test_geometry.cpp.

struct Point {
	double x = 0, y = 0;

	bool operator==(const Point &) const = default;
};

// Canvas-space rectangle.
struct Rect {
	double x = 0, y = 0, w = 0, h = 0;

	constexpr double cx() const { return x + w / 2; }
	constexpr double cy() const { return y + h / 2; }
	bool contains(double px, double py) const;
	bool intersects(const Rect &other) const;
	double overlap_area(const Rect &other) const;
	Rect united(const Rect &other) const;

	bool operator==(const Rect &) const = default;
};

// A view onto the canvas: the canvas point shown at the viewport centre and the
// magnification (1 = one canvas unit per logical pixel).
struct Camera {
	double cx = 0, cy = 0, zoom = 1;

	// Canvas rectangle visible on a vw x vh logical-pixel viewport.
	Rect viewport(double vw, double vh) const;

	bool operator==(const Camera &) const = default;
};

// Compass direction: each component -1, 0 or 1.
struct Compass {
	int x = 0, y = 0;

	bool operator==(const Compass &) const = default;
};

// lo wins when hi < lo.
constexpr double clampd(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

double ease_out_cubic(double t);

// Centre moves linearly, zoom geometrically; t in [0, 1]. Used for pans.
Camera camera_lerp(const Camera &a, const Camera &b, double t);

// Zoom transition between a close-up camera `near` (t = 0) and a zoomed-out camera `far`
// (t = 1). Zoom changes geometrically while the region `near` shows slides linearly on
// screen from the centre to where `far` displays it, so zooming out visibly shrinks the
// current screen into its place on the canvas (and zooming in grows the target out of it).
Camera camera_zoom_path(const Camera &near, const Camera &far, double t);

// Largest zoom (at most max_zoom) that shows all of `content` inside a vw x vh viewport
// with `margin` logical pixels free on every side.
double fit_zoom(const Rect &content, double vw, double vh, double margin, double max_zoom);

// How many canvas units `px` output pixels cover of an area shown at `zoom`,
// rounded up so the area clears them (an exact quotient stays exact): the
// part of a scaled desktop under a bar.
int canvas_units_covered(int px, double zoom);

// The candidate closest to (ox, oy) in direction (dx, dy): its centre must lie within
// max_angle_deg of the direction, and distance off-axis is penalised so the most "straight
// ahead" neighbour wins over a slightly nearer diagonal one. `exclude` is skipped.
std::optional<std::size_t> nav_pick_direction(double ox, double oy, double dx, double dy,
	std::span<const Rect> candidates, std::optional<std::size_t> exclude,
	double max_angle_deg);

// Snaps a direction to one of the 8 compass directions.
Compass nav_quantize_direction(double dx, double dy);

// Grid of n cells filling `area`, `gap` between and around cells. Row-major; the last
// row's cells widen to fill the row.
std::vector<Rect> tile_layout(const Rect &area, std::size_t n, double gap);

// Order of frames for tile_layout: rows (frame centres banded by `band` canvas units)
// top to bottom, left to right within a row. Returns indices into `frames`.
std::vector<std::size_t> tile_order(std::span<const Rect> frames, double band);
