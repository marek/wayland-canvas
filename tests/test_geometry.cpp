// Unit tests for src/core/geometry.cpp. Self-contained: no framework, links only
// geometry.cpp. Run via `meson test` (test 'geometry').

#include <cmath>
#include <cstdio>
#include <optional>
#include <source_location>

#include "core/geometry.h"

namespace {

int failures = 0;

void check(bool condition, std::source_location location = std::source_location::current()) {
	if (!condition) {
		++failures;
		std::printf("FAIL %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
	}
}

void check_near(double got, double expected, double tol,
	std::source_location location = std::source_location::current()) {
	if (!(std::fabs(got - expected) <= tol)) {
		++failures;
		std::printf("FAIL %s:%u: %.17g, expected %.17g (tol %.17g)\n",
			location.file_name(), static_cast<unsigned>(location.line()), got, expected, tol);
	}
}

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

// ---- tile_layout ----

void test_tile_layout() {
	// 5 cells in a 100x60 area with gap 2: 3 columns, 2 rows.
	const Rect area = {0, 0, 100, 60};
	std::vector<Rect> cells = tile_layout(area, 5, 2);

	double w0 = 92.0 / 3.0; // (100 - 2*(3+1)) / 3
	double h = 27.0;        // (60 - 2*(2+1)) / 2
	// Row 0: three equally sized cells.
	check_near(cells[0].x, 2, 1e-9);
	check_near(cells[0].y, 2, 1e-9);
	check_near(cells[0].w, w0, 1e-9);
	check_near(cells[0].h, h, 1e-9);
	check_near(cells[1].x, 2 + (w0 + 2), 1e-9);
	check_near(cells[1].y, 2, 1e-9);
	check_near(cells[1].w, w0, 1e-9);
	check_near(cells[1].h, h, 1e-9);
	check_near(cells[2].x, 2 + 2 * (w0 + 2), 1e-9);
	check_near(cells[2].y, 2, 1e-9);
	check_near(cells[2].w, w0, 1e-9);
	check_near(cells[2].h, h, 1e-9);
	// Row 1: only two cells, widened to fill the row.
	check_near(cells[3].x, 2, 1e-9);
	check_near(cells[3].y, 2 + (h + 2), 1e-9);
	check_near(cells[3].w, 47.0, 1e-9); // (100 - 2*(2+1)) / 2
	check_near(cells[3].h, h, 1e-9);
	check_near(cells[4].x, 2 + (47.0 + 2), 1e-9);
	check_near(cells[4].y, cells[3].y, 1e-9);
	check_near(cells[4].w, 47.0, 1e-9);
	check_near(cells[4].h, h, 1e-9);
	check(cells[3].w > cells[0].w + 1);

	// Counts per row: rows share a y band, three then two.
	check(cells[0].y == cells[1].y && cells[1].y == cells[2].y);
	check(cells[3].y == cells[4].y);
	check(cells[2].y != cells[3].y);

	// No pair of cells overlaps...
	for (int i = 0; i < 5; i++) {
		for (int j = i + 1; j < 5; j++) {
			check(!cells[i].intersects(cells[j]));
		}
	}
	// ...and every cell sits inside the area with the gap respected.
	for (int i = 0; i < 5; i++) {
		check(cells[i].x >= 2);
		check(cells[i].y >= 2);
		check(cells[i].x + cells[i].w <= 98 + 1e-9);
		check(cells[i].y + cells[i].h <= 58 + 1e-9);
	}
	// Horizontal neighbours are exactly one gap apart.
	check_near(cells[1].x - cells[0].x - cells[0].w, 2, 1e-9);
	check_near(cells[3].y - cells[0].y - cells[0].h, 2, 1e-9);

	// A single cell fills the area minus the gap on every side.
	std::vector<Rect> one = tile_layout(area, 1, 2);
	check_near(one[0].x, 2, 1e-9);
	check_near(one[0].y, 2, 1e-9);
	check_near(one[0].w, 96, 1e-9);
	check_near(one[0].h, 56, 1e-9);

	// Perfect square count: 2x2, no widening anywhere.
	std::vector<Rect> four = tile_layout(area, 4, 2);
	for (int i = 0; i < 4; i++) {
		check_near(four[i].w, 47.0, 1e-9);
		check_near(four[i].h, 27.0, 1e-9);
	}
	check_near(four[0].x, 2, 1e-9);
	check_near(four[1].x, 51, 1e-9);
	check_near(four[2].x, 2, 1e-9);
	check_near(four[3].x, 51, 1e-9);
	check_near(four[0].y, 2, 1e-9);
	check_near(four[2].y, 31, 1e-9);
}

// ---- tile_order ----

void test_tile_order() {
	// Centres: (150,50) (10,50) (100,150) (20,260) (200,50); band 100 gives
	// rows 0, 0, 1, 2, 0. Expected: row 0 by x (10, 150, 200), then rows
	// 1 and 2 top to bottom.
	const Rect frames[5] = {
		{145, 45, 10, 10},
		{5, 45, 10, 10},
		{95, 145, 10, 10},
		{15, 255, 10, 10},
		{195, 45, 10, 10},
	};
	std::vector<std::size_t> order = tile_order(frames, 100);
	check(order[0] == 1);
	check(order[1] == 0);
	check(order[2] == 4);
	check(order[3] == 2);
	check(order[4] == 3);

	// Same frames shuffled on input: banding still groups rows first.
	const Rect shuffled[5] = {
		{95, 145, 10, 10},  // row 1
		{195, 45, 10, 10},  // row 0, x 200
		{145, 45, 10, 10},  // row 0, x 150
		{15, 255, 10, 10},  // row 2
		{5, 45, 10, 10},    // row 0, x 10
	};
	order = tile_order(shuffled, 100);
	check(order[0] == 4);
	check(order[1] == 2);
	check(order[2] == 1);
	check(order[3] == 0);
	check(order[4] == 3);

	// Two frames in the same row keep left-to-right order; equal centres
	// are stable (earlier index first).
	const Rect pair[2] = {{30, 0, 10, 10}, {0, 5, 10, 10}};
	order = tile_order(pair, 100);
	check(order[0] == 1);
	check(order[1] == 0);
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

} // namespace

int main() {
	test_nav_pick_direction();
	test_nav_quantize_direction();
	test_tile_layout();
	test_tile_order();
	test_camera_zoom_path();
	test_camera_viewport();
	test_fit_zoom();
	test_basics();

	if (failures == 0) {
		std::printf("all geometry tests passed\n");
	} else {
		std::printf("%d geometry test failure(s)\n", failures);
	}
	return failures != 0;
}
