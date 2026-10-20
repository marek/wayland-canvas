// Unit tests for src/core/geometry.cpp. Self-contained: no framework, links only
// geometry.cpp. Run via `meson test` (test 'geometry').

#include <optional>

#include "check.h"
#include "core/geometry.h"

namespace {

using unit::check;
using unit::check_near;

// ---- nav_pick_direction ----

void test_nav_pick_direction() {
	// Origin at (0,0), swiping east. Candidates (10x10 rects, centres in
	// parentheses):
	//   0: (50, 30)  nearer than 1, ~31 deg off-axis, inside the 60 deg cone
	//   1: (100, 0)  straight ahead, but farther away
	//   2: (10, 30)  nearest of all, ~72 deg off-axis, beyond the cone
	const Rect cands[3] = {
		{45, 25, 10, 10},
		{95, -5, 10, 10},
		{5, 25, 10, 10},
	};
	// Straight-ahead beats the nearer off-axis candidate; the beyond-cone
	// one is never considered.
	check(nav_pick_direction(0, 0, 1, 0, cands, std::nullopt, 60.0) == 1);
	// Excluding the straight-ahead one falls back to the off-axis one.
	check(nav_pick_direction(0, 0, 1, 0, cands, 1, 60.0) == 0);
	check(nav_pick_direction(0, 0, 1, 0, cands, 0, 60.0) == 1);
	check(nav_pick_direction(0, 0, 1, 0, cands, 2, 60.0) == 1);
	const Rect only_off[] = {{45, 25, 10, 10}, {5, 25, 10, 10}};
	check(nav_pick_direction(0, 0, 1, 0, only_off, 0, 60.0) == std::nullopt);

	// Nothing ahead: everything lies west (behind the direction).
	const Rect behind[] = {{-205, -5, 10, 10}, {-100, 0, 10, 10}};
	check(nav_pick_direction(0, 0, 1, 0, behind, std::nullopt, 60.0) == std::nullopt);
	// Opposite direction: the east candidates are all behind then.
	check(nav_pick_direction(0, 0, -1, 0, cands, std::nullopt, 60.0) == std::nullopt);
	// Empty candidate list and zero-length direction.
	check(nav_pick_direction(0, 0, 1, 0, {}, std::nullopt, 60.0) == std::nullopt);
	check(nav_pick_direction(0, 0, 0, 0, cands, std::nullopt, 60.0) == std::nullopt);

	// Vertical direction with a tilted-but-in-cone candidate.
	const Rect up_cands[] = {{-200, 100, 10, 10}, {0, 100, 10, 10}};
	check(nav_pick_direction(0, 0, 0, 1, up_cands, std::nullopt, 60.0) == 1);
	check(nav_pick_direction(0, 0, 0, 1, up_cands, 1, 60.0) == std::nullopt);
}

// ---- nav_quantize_direction ----

void test_nav_quantize_direction() {
	// The 8 compass directions, including the 4 exact 45 deg diagonals
	// (ties between two compass directions land on the diagonal).
	const double dirs[8][2] = {
		{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
	};
	for (int i = 0; i < 8; i++) {
		Compass c = nav_quantize_direction(dirs[i][0], dirs[i][1]);
		check(c.x == static_cast<int>(dirs[i][0]));
		check(c.y == static_cast<int>(dirs[i][1]));
	}
	// Off-axis inputs snap to the nearest compass sector.
	Compass c = nav_quantize_direction(1, 0.3);
	check(c.x == 1 && c.y == 0);
	c = nav_quantize_direction(1, 0.6);
	check(c.x == 1 && c.y == 1);
	c = nav_quantize_direction(0.3, 1);
	check(c.x == 0 && c.y == 1);
	c = nav_quantize_direction(0.6, 1);
	check(c.x == 1 && c.y == 1);
	c = nav_quantize_direction(-2, 1);
	check(c.x == -1 && c.y == 1);
	c = nav_quantize_direction(2, -1);
	check(c.x == 1 && c.y == -1);
	c = nav_quantize_direction(-1, -0.6);
	check(c.x == -1 && c.y == -1);
	c = nav_quantize_direction(-0.3, -1);
	check(c.x == 0 && c.y == -1);
	// Unnormalized magnitudes do not matter.
	c = nav_quantize_direction(100, 0);
	check(c.x == 1 && c.y == 0);
	c = nav_quantize_direction(0, -50);
	check(c.x == 0 && c.y == -1);
	// Zero vector quantizes to zero.
	c = nav_quantize_direction(0, 0);
	check(c.x == 0 && c.y == 0);
}

// ---- slices ----

void test_step_slice() {
	const Slice whole{0, 0, 1, 1};
	const Slice left{0, 0, 0.5, 1};
	const Slice right{0.5, 0, 1, 1};
	const Slice top{0, 0, 1, 0.5};
	const Slice bottom{0, 0.5, 1, 1};
	const Slice bl{0, 0.5, 0.5, 1};
	const Slice tr{0.5, 0, 1, 0.5};
	// Super+Left: the second half becomes the whole, the whole the left half,
	// the left half stays.
	check(step_slice(right, 0, -1) == whole);
	check(step_slice(whole, 0, -1) == left);
	check(step_slice(left, 0, -1) == left);
	// Super+Right, the other way around on x.
	check(step_slice(left, 0, 1) == whole);
	check(step_slice(whole, 0,1) == right);
	check(step_slice(right, 0,1) == right);
	// Super+Up and Super+Down alike on y.
	check(step_slice(bottom, 1, -1) == whole);
	check(step_slice(whole, 1, -1) == top);
	check(step_slice(top, 1, -1) == top);
	check(step_slice(top, 1,1) == whole);
	check(step_slice(whole, 1,1) == bottom);
	check(step_slice(bottom, 1,1) == bottom);
	// The other axis keeps its span: a quarter stepped along its own half's
	// axis does not move, stepped along the other one it widens to the half.
	check(step_slice(bl, 0, -1) == bl); // already at the near half: nothing to do
	check(step_slice(tr, 0, 1) == tr); // already at the far half: nothing to do
	check(step_slice(bl, 1, -1) == left); // bottom-left quarter widens to the left half
	check(step_slice(tr, 1,1) == right);
	// A slice made only by halving counts as the half it lies in.
	check(step_slice(Slice{0.25, 0, 0.5, 0.5}, 0,1) == top);
	check(step_slice(Slice{0, 0.25, 0.5, 0.5}, 1, 1) == left);
	check(step_slice(Slice{0, 0.5, 0.25, 1}, 0, 1) == bottom);
}

void test_slice_spans() {
	check(Slice{0, 0, 1, 1}.span_x() == 1);
	check(Slice{0, 0, 1, 1}.span_y() == 1);
	check(Slice{0, 0, 0.5, 1}.span_x() == 0);
	check(Slice{0.5, 0, 1, 1}.span_x() == 2);
	check(Slice{0, 0, 1, 0.5}.span_y() == 0);
	check(Slice{0, 0.5, 1, 1}.span_y() == 2);
	// Halving-made slices still lie in one half on the halved axis.
	check(Slice{0.25, 0, 0.5, 0.5}.span_x() == 0);
	check(Slice{0, 0.25, 0.5, 0.5}.span_y() == 0);
	// Containment takes the point on its low edges, not its high ones.
	check(Slice{0, 0, 0.5, 1}.contains(0, 0));
	check(Slice{0, 0, 0.5, 1}.contains(0.49, 0.99));
	check(!Slice{0, 0, 0.5, 1}.contains(0.5, 0.5));
	check(!Slice{0, 0, 0.5, 1}.contains(1, 1));
	// Touching edges are not overlap.
	check(!Slice{0, 0, 0.5, 1}.intersects(Slice{0.5, 0, 1, 1}));
	check(!Slice{0, 0, 1, 0.5}.intersects(Slice{0, 0.5, 1, 1}));
	check(Slice{0, 0, 0.5, 1}.intersects(Slice{0.25, 0.25, 0.75, 0.75}));
}

void test_reading_order() {
	// Rows top to bottom, left to right within a row.
	check(reading_before(Slice{0, 0, 0.5, 0.5}, Slice{0.5, 0, 1, 0.5}));
	check(reading_before(Slice{0.5, 0, 1, 0.5}, Slice{0, 0.5, 0.5, 1}));
	check(reading_before(Slice{0, 0.5, 0.5, 1}, Slice{0.5, 0.5, 1, 1}));
	// Same top-left corner: the slice that ends first is read first, so the
	// top half comes before the left half.
	check(reading_before(Slice{0, 0, 1, 0.5}, Slice{0, 0, 0.5, 1}));
	check(reading_before(Slice{0.5, 0, 1, 1}, Slice{0, 0.5, 1, 1}));
	check(!reading_before(Slice{0.5, 0, 1, 1}, Slice{0, 0, 1, 0.5}));
}

void test_halve_slice() {
	// A landscape or square slice splits left/right, a portrait one top/bottom.
	auto [kept_l, freed_l] = halve_slice(Slice{0, 0, 1, 0.5});
	check(kept_l == Slice{0, 0, 0.5, 0.5});
	check(freed_l == Slice{0.5, 0, 1, 0.5});
	auto [kept_s, freed_s] = halve_slice(Slice{0, 0, 0.5, 0.5});
	check(kept_s == Slice{0, 0, 0.25, 0.5});
	check(freed_s == Slice{0.25, 0, 0.5, 0.5});
	auto [kept_t, freed_t] = halve_slice(Slice{0, 0, 0.5, 1});
	check(kept_t == Slice{0, 0, 0.5, 0.5});
	check(freed_t == Slice{0, 0.5, 0.5, 1});
}

void test_largest_free_slice() {
	// An empty board is all free: the whole board is the largest free slice.
	check(largest_free_slice(std::vector<Slice>{}) == Slice{0, 0,1,1});

	// One occupied quarter leaves three: both cross halves of equal area tie,
	// and the earlier-read top half wins over the left half.
	const Slice bl{0, 0.5, 0.5, 1};
	check(largest_free_slice(std::vector<Slice>{bl}) == Slice{0, 0, 1, 0.5});

	// A full board leaves no free slice at all.
	const Slice left{0, 0,0.5,1}, right{0.5, 0, 1, 1};
	check(!largest_free_slice(std::vector<Slice>{left, right}).has_value());

	// Free space split into two diagonal quarters: equal areas, reading order
	// picks the top-right one over the bottom-left one.
	check(largest_free_slice(std::vector<Slice>{Slice{0, 0, 0.5, 0.5}, Slice{0.5, 0.5, 1, 1}})
		== Slice{0.5, 0, 1, 0.5});

	// Halving debris: a free eighth beside an occupied one; the eighths pair up
	// into the half they lie in only when both are free, so the free quarter on
	// the other side is the largest.
	check(largest_free_slice(std::vector<Slice>{Slice{0, 0, 0.25, 0.5}})
		== Slice{0.5, 0, 1, 1});

	// The `under` point narrows the search to the slices containing it.
	check(largest_free_slice(std::vector<Slice>{bl}, Point{.x = 0.75, .y = 0.25})
		== Slice{0, 0, 1, 0.5});
	check(largest_free_slice(std::vector<Slice>{bl}, Point{.x = 0.75, .y = 0.75})
		== Slice{0.5, 0, 1, 1});
	check(!largest_free_slice(std::vector<Slice>{bl}, Point{.x = 0.25, .y = 0.75}).has_value());
}

void test_place_new() {
	const Slice left_half{0, 0, 0.5,1}, right_half{0.5, 0, 1,1};
	std::optional<SlicePlacement> p = place_new(std::vector<Slice>{});
	check(p.has_value() && p->slice == Slice{0, 0, 1, 1} && p->halved == no_slice);

	// One window holding the whole board: the newcomer halves it, the holder
	// keeping the left half.
	p = place_new(std::vector<Slice>{Slice{0, 0, 1, 1}});
	check(p.has_value() && p->slice == Slice{0.5, 0, 1, 1} && p->halved == 0);

	// Halves held by two windows: the newcomer halves the earlier-read left
	// half, top/bottom because it is portrait.
	p = place_new(std::vector<Slice>{left_half, right_half});
	check(p.has_value() && p->slice == Slice{0, 0.5, 0.5, 1} && p->halved == 0);

	// Free space is taken before anything is halved: the two diagonal
	// quarters leave the top-left one free.
	p = place_new(std::vector<Slice>{Slice{0,0.5,0.5,1}, Slice{0.5, 0, 1, 0.5}});
	check(p.has_value() && p->slice == Slice{0, 0, 0.5, 0.5} && p->halved == no_slice);

	// Pushing without free space halves the largest slice other than the
	// moved one; the moved one is excluded by index.
	p = place_new(std::vector<Slice>{left_half, right_half}, 1);
	check(p.has_value() && p->halved == 0
		&& p->slice == Slice{0, 0.5, 0.5, 1});

	// Nothing to halve but the moved slice itself: the push halves it back.
	p = place_new(std::vector<Slice>{Slice{0,0, 1, 1}}, 0);
	check(p.has_value() && p->halved == 0 && p->slice == Slice{0.5, 0, 1, 1});

	// A crowded board, nothing free: the largest slice is halved, the
	// earliest-read of the equal largest, along its longer side.
	const Slice tl_left{0, 0, 0.25, 0.5}, tl_eighth{0.25, 0, 0.5, 0.5},
		tr{0.5, 0, 1, 0.5}, bl{0, 0.5, 0.5, 1}, br{0.5, 0.5, 1, 1};
	p = place_new(std::vector<Slice>{tl_left, tl_eighth, tr, bl, br});
	check(p.has_value() && p->halved == 2 && p->slice == Slice{0.75, 0, 1, 0.5});
}

void test_slice_box() {
	const Rect area{10, 20, 100, 60};
	Rect whole = slice_box(area, Slice{0, 0, 1, 1}, 2);
	check_near(whole.x, 11, 1e-9);
	check_near(whole.y, 21, 1e-9);
	check_near(whole.w, 98, 1e-9);
	check_near(whole.h, 58, 1e-9);
	Rect half = slice_box(area, Slice{0, 0, 0.5, 1}, 2);
	check_near(half.x, 11, 1e-9);
	check_near(half.y, 21, 1e-9);
	check_near(half.w, 48, 1e-9);
	check_near(half.h, 58, 1e-9);
	Rect quarter = slice_box(area, Slice{0.5, 0.5, 1, 1}, 2);
	check_near(quarter.x, 61, 1e-9);
	check_near(quarter.y, 51, 1e-9);
	check_near(quarter.w, 48, 1e-9);
	check_near(quarter.h, 28, 1e-9);
}

// ---- camera_zoom_path ----

void test_camera_zoom_path() {
	const Camera near_cam = {.cx = 100, .cy = 200, .zoom = 1};
	const Camera far_cam = {.cx = 500, .cy = 300, .zoom = 0.25};

	// Endpoints: t = 0 is exactly `near`, t = 1 is `far`.
	Camera c0 = camera_zoom_path(near_cam, far_cam, 0);
	check(c0.cx == 100);
	check(c0.cy == 200);
	check(c0.zoom == 1);
	Camera c1 = camera_zoom_path(near_cam, far_cam, 1);
	check_near(c1.cx, 500, 1e-9);
	check_near(c1.cy, 300, 1e-9);
	check_near(c1.zoom, 0.25, 1e-9);

	// Zoom moves geometrically: strictly decreasing, inside the endpoints,
	// and the geometric mean at the halfway point.
	double prev = near_cam.zoom;
	for (int i = 1; i < 20; i++) {
		Camera c = camera_zoom_path(near_cam, far_cam, i / 20.0);
		check(c.zoom < prev);
		check(c.zoom > 0.25);
		prev = c.zoom;
	}
	Camera mid = camera_zoom_path(near_cam, far_cam, 0.5);
	check_near(mid.zoom, 0.5, 1e-9);
	// Near's centre slides toward its place in `far`: at t = 1 the screen
	// centre lands exactly on `far`'s centre.
	check(mid.cx > near_cam.cx && mid.cx < far_cam.cx);
	check(mid.cy > near_cam.cy && mid.cy < far_cam.cy);
}

// ---- camera_viewport / fit_zoom ----

void test_camera_viewport() {
	Rect vp = Camera{100, 200, 2}.viewport(800, 600);
	check(vp.x == -100);
	check(vp.y == 50);
	check(vp.w == 400);
	check(vp.h == 300);

	vp = Camera{0, 0, 0.5}.viewport(1000, 500);
	check(vp.x == -1000);
	check(vp.y == -500);
	check(vp.w == 2000);
	check(vp.h == 1000);

	// Zoom 1 on a vw x vh screen shows exactly vw x vh canvas units.
	vp = Camera{7, 9, 1}.viewport(640, 480);
	check(vp.w == 640);
	check(vp.h == 480);
	check(vp.x == 7 - 320);
	check(vp.y == 9 - 240);
}

void test_fit_zoom() {
	// Width-limited: (800-100)/1400 = 0.5, (600-100)/1000 = 0.5.
	check(fit_zoom(Rect{0, 0, 1400, 1000}, 800, 600, 50, 1) == 0.5);
	// Height-limited: (600-100)/2000 = 0.25 beats (800-100)/1400 = 0.5.
	check(fit_zoom(Rect{0, 0, 1400, 2000}, 800, 600, 50, 1) == 0.25);
	// Both axes agree at 0.25 for 2800x2000.
	check(fit_zoom(Rect{0, 0, 2800, 2000}, 800, 600, 50, 1) == 0.25);
	// Small content is capped by max_zoom.
	check(fit_zoom(Rect{0, 0, 100, 100}, 800, 600, 50, 0.3) == 0.3);
	// Empty content never shrinks the zoom below max_zoom.
	check(fit_zoom(Rect{0, 0, 0, 0}, 800, 600, 50, 0.4) == 0.4);
	// Margin eats into the available area.
	check(fit_zoom(Rect{0, 0, 700, 500}, 800, 600, 50, 1) == 1);
	check(fit_zoom(Rect{0, 0, 701, 500}, 800, 600, 50, 1) < 1);
	// A margin larger than the viewport clamps to a 1px area.
	check(fit_zoom(Rect{0, 0, 100, 100}, 100, 100, 80, 1) < 0.02);
}

// ---- small helpers used by the above ----

void test_basics() {
	check(clampd(5, 0, 3) == 3);
	check(clampd(-1, 0, 3) == 0);
	check(clampd(2, 0, 3) == 2);
	check(clampd(2, 3, 0) == 3); // hi < lo: the lo bound wins

	check(ease_out_cubic(0) == 0);
	check(ease_out_cubic(1) == 1);
	check(ease_out_cubic(0.5) == 0.875);
	check(ease_out_cubic(-1) == 0);
	check(ease_out_cubic(2) == 1);
	check(ease_out_cubic(0.25) == 0.578125);

	check(Rect{0, 0, 10, 10}.contains(0, 0));
	check(Rect{0, 0, 10, 10}.contains(9.999, 9.999));
	check(!Rect{0, 0, 10, 10}.contains(10, 5));
	check(!Rect{0, 0, 10, 10}.contains(-0.001, 5));

	check(Rect{0, 0, 10, 10}.overlap_area(Rect{5, 5, 10, 10}) == 25);
	check(Rect{0, 0, 10, 10}.overlap_area(Rect{10, 0, 10, 10}) == 0);
	check(Rect{0, 0, 10, 10}.overlap_area(Rect{0, 0, 4, 4}) == 16);

	Rect u = Rect{0, 0, 10, 10}.united(Rect{5, 5, 10, 10});
	check(u.x == 0 && u.y == 0 && u.w == 15 && u.h == 15);

	// camera_lerp: centres linear, zoom geometric.
	const Camera a = {.cx = 0, .cy = 0, .zoom = 1};
	const Camera b = {.cx = 100, .cy = -40, .zoom = 0.25};
	Camera m = camera_lerp(a, b, 0.5);
	check(m.cx == 50);
	check(m.cy == -20);
	check_near(m.zoom, 0.5, 1e-9);
	Camera e0 = camera_lerp(a, b, 0);
	Camera e1 = camera_lerp(a, b, 1);
	check(e0.cx == 0 && e0.cy == 0 && e0.zoom == 1);
	check_near(e1.cx, 100, 1e-9);
	check_near(e1.cy, -40, 1e-9);
	check_near(e1.zoom, 0.25, 1e-9);
}

// A 26 px bar over a desktop shown at its scale: the canvas it covers.
void test_canvas_units_covered() {
	check(canvas_units_covered(26, 1.0) == 26);
	check(canvas_units_covered(26, 0.5) == 52);  // at 50 % the bar covers twice the canvas
	check(canvas_units_covered(26, 2.0) == 13);
	check(canvas_units_covered(26, 0.8) == 33);  // 32.5 rounds up: the area clears the bar
	check(canvas_units_covered(26, 1.25) == 21); // 20.8 rounds up
	check(canvas_units_covered(26, 1.3) == 20);  // 20.000...04 is exact, not 21
	check(canvas_units_covered(0, 0.5) == 0);    // no bar, no inset
}

} // namespace

int main() {
	test_nav_pick_direction();
	test_nav_quantize_direction();
	test_step_slice();
	test_slice_spans();
	test_reading_order();
	test_halve_slice();
	test_largest_free_slice();
	test_place_new();
	test_slice_box();
	test_camera_zoom_path();
	test_camera_viewport();
	test_fit_zoom();
	test_canvas_units_covered();
	test_basics();

	return unit::summary();
}
