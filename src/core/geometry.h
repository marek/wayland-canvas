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

// A slice of a board: the fractions of its area the slice spans, [x0,x1) x [y0,y1).
// Every slice lies inside one half of the board on each axis or spans it whole,
// because halving never carries a piece across the board's middle.
struct Slice {
	double x0 = 0, y0 = 0, x1 = 1, y1 = 1;

	bool operator==(const Slice &) const = default;
	bool intersects(const Slice &other) const; // strict overlap; touching edges do not
	bool contains(double px, double py) const;
	// The half of the board the slice lies in on one axis: 0 first, 1 whole, 2 second.
	// A slice made only by halving still lies in one half, so it counts as that half.
	int span_x() const;
	int span_y() const;
};

// Steps a slice toward `dir` (-1 first/left/up, +1 second/right/down) on one axis
// (0 x, 1 y): the far half becomes the whole, the whole the near half, the near
// half stays; the other axis keeps its span. The result is a half-aligned slice,
// whatever halvings made the input.
Slice step_slice(Slice slice, int axis, int dir);

// Order to read slices in: rows top to bottom, left to right within a row.
bool reading_before(const Slice &a, const Slice &b);

// Splits a slice along its longer side; a landscape or square one splits left/right.
// The first half is the left/top one a holder keeps when halved.
std::pair<Slice, Slice> halve_slice(Slice slice);

// The largest slice not overlapping any in `occupied`, containing the fraction
// point `under` when given: the whole board, its four halves and, below them,
// halves of those along their longer side, down to where the occupied slices
// leave room. Ties go to the earlier-read slice. No room: nullopt.
std::optional<Slice> largest_free_slice(std::span<const Slice> occupied,
	std::optional<Point> under = std::nullopt);

inline constexpr std::size_t no_slice = static_cast<std::size_t>(-1);

// Where a new window goes: the largest free slice or, with none free, half of
// the largest slice other than `exclude` (the one a key moved; when it is the
// only one, it is halved back rather than covered). `halved` names the
// occupied entry that was halved, no_slice when a free slice was taken.
struct SlicePlacement {
	Slice slice;
	std::size_t halved = no_slice;
};
std::optional<SlicePlacement> place_new(std::span<const Slice> occupied,
	std::optional<std::size_t> exclude = std::nullopt);

// A slice's window box: the slice laid into `area`, inset by half the gap on
// every side so neighbours sit a full gap apart.
Rect slice_box(const Rect &area, const Slice &slice, double gap);
