/* Unit tests for src/geometry.c. Self-contained: no framework, links only
 * geometry.c. Run via `meson test` (test 'geometry'). */

#include <math.h>
#include <stdio.h>
#include "geometry.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_NEAR(a, b, tol) do { \
	double va_ = (a), vb_ = (b); \
	if (!(fabs(va_ - vb_) <= (tol))) { \
		failures++; \
		printf("FAIL %s:%d: %s = %.17g, expected %.17g (tol %.17g)\n", \
			__FILE__, __LINE__, #a, va_, vb_, (double)(tol)); \
	} \
} while (0)

/* ---- nav_pick_direction ---- */

static void test_nav_pick_direction(void) {
	/* Origin at (0,0), swiping east. Candidates (10x10 rects, centres in
	 * parentheses):
	 *   0: (50, 30)  nearer than 1, ~31 deg off-axis, inside the 60 deg cone
	 *   1: (100, 0)  straight ahead, but farther away
	 *   2: (10, 30)  nearest of all, ~72 deg off-axis, beyond the cone */
	struct rect cands[3] = {
		{45, 25, 10, 10},
		{95, -5, 10, 10},
		{5, 25, 10, 10},
	};
	/* Straight-ahead beats the nearer off-axis candidate; the beyond-cone
	 * one is never considered. */
	CHECK(nav_pick_direction(0, 0, 1, 0, cands, 3, -1, 60.0) == 1);
	/* Excluding the straight-ahead one falls back to the off-axis one. */
	CHECK(nav_pick_direction(0, 0, 1, 0, cands, 3, 1, 60.0) == 0);
	CHECK(nav_pick_direction(0, 0, 1, 0, cands, 3, 0, 60.0) == 1);
	CHECK(nav_pick_direction(0, 0, 1, 0, cands, 3, 2, 60.0) == 1);
	struct rect only_off[] = {{45, 25, 10, 10}, {5, 25, 10, 10}};
	CHECK(nav_pick_direction(0, 0, 1, 0, only_off, 2, 0, 60.0) == -1);

	/* Nothing ahead: everything lies west (behind the direction). */
	struct rect behind[] = {{-205, -5, 10, 10}, {-100, 0, 10, 10}};
	CHECK(nav_pick_direction(0, 0, 1, 0, behind, 2, -1, 60.0) == -1);
	/* Opposite direction: the east candidates are all behind then. */
	CHECK(nav_pick_direction(0, 0, -1, 0, cands, 3, -1, 60.0) == -1);
	/* Empty candidate list and zero-length direction. */
	CHECK(nav_pick_direction(0, 0, 1, 0, cands, 0, -1, 60.0) == -1);
	CHECK(nav_pick_direction(0, 0, 0, 0, cands, 3, -1, 60.0) == -1);

	/* Vertical direction with a tilted-but-in-cone candidate. */
	struct rect up_cands[] = {{-200, 100, 10, 10}, {0, 100, 10, 10}};
	CHECK(nav_pick_direction(0, 0, 0, 1, up_cands, 2, -1, 60.0) == 1);
	CHECK(nav_pick_direction(0, 0, 0, 1, up_cands, 2, 1, 60.0) == -1);
}

/* ---- nav_quantize_direction ---- */

static void test_nav_quantize_direction(void) {
	/* The 8 compass directions, including the 4 exact 45 deg diagonals
	 * (ties between two compass directions land on the diagonal). */
	const double dirs[8][2] = {
		{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
	};
	for (int i = 0; i < 8; i++) {
		int qx = 99, qy = 99;
		nav_quantize_direction(dirs[i][0], dirs[i][1], &qx, &qy);
		CHECK(qx == (int)dirs[i][0]);
		CHECK(qy == (int)dirs[i][1]);
	}
	/* Off-axis inputs snap to the nearest compass sector. */
	int qx, qy;
	nav_quantize_direction(1, 0.3, &qx, &qy);
	CHECK(qx == 1 && qy == 0);
	nav_quantize_direction(1, 0.6, &qx, &qy);
	CHECK(qx == 1 && qy == 1);
	nav_quantize_direction(0.3, 1, &qx, &qy);
	CHECK(qx == 0 && qy == 1);
	nav_quantize_direction(0.6, 1, &qx, &qy);
	CHECK(qx == 1 && qy == 1);
	nav_quantize_direction(-2, 1, &qx, &qy);
	CHECK(qx == -1 && qy == 1);
	nav_quantize_direction(2, -1, &qx, &qy);
	CHECK(qx == 1 && qy == -1);
	nav_quantize_direction(-1, -0.6, &qx, &qy);
	CHECK(qx == -1 && qy == -1);
	nav_quantize_direction(-0.3, -1, &qx, &qy);
	CHECK(qx == 0 && qy == -1);
	/* Unnormalized magnitudes do not matter. */
	nav_quantize_direction(100, 0, &qx, &qy);
	CHECK(qx == 1 && qy == 0);
	nav_quantize_direction(0, -50, &qx, &qy);
	CHECK(qx == 0 && qy == -1);
	/* Zero vector quantizes to zero. */
	nav_quantize_direction(0, 0, &qx, &qy);
	CHECK(qx == 0 && qy == 0);
}

/* ---- tile_layout ---- */

static void test_tile_layout(void) {
	/* 5 cells in a 100x60 area with gap 2: 3 columns, 2 rows. */
	struct rect area = {0, 0, 100, 60};
	struct rect cells[5];
	tile_layout(area, 5, 2, cells);

	double w0 = 92.0 / 3.0; /* (100 - 2*(3+1)) / 3 */
	double h = 27.0;        /* (60 - 2*(2+1)) / 2 */
	/* Row 0: three equally sized cells. */
	CHECK_NEAR(cells[0].x, 2, 1e-9);
	CHECK_NEAR(cells[0].y, 2, 1e-9);
	CHECK_NEAR(cells[0].w, w0, 1e-9);
	CHECK_NEAR(cells[0].h, h, 1e-9);
	CHECK_NEAR(cells[1].x, 2 + (w0 + 2), 1e-9);
	CHECK_NEAR(cells[1].y, 2, 1e-9);
	CHECK_NEAR(cells[1].w, w0, 1e-9);
	CHECK_NEAR(cells[1].h, h, 1e-9);
	CHECK_NEAR(cells[2].x, 2 + 2 * (w0 + 2), 1e-9);
	CHECK_NEAR(cells[2].y, 2, 1e-9);
	CHECK_NEAR(cells[2].w, w0, 1e-9);
	CHECK_NEAR(cells[2].h, h, 1e-9);
	/* Row 1: only two cells, widened to fill the row. */
	CHECK_NEAR(cells[3].x, 2, 1e-9);
	CHECK_NEAR(cells[3].y, 2 + (h + 2), 1e-9);
	CHECK_NEAR(cells[3].w, 47.0, 1e-9); /* (100 - 2*(2+1)) / 2 */
	CHECK_NEAR(cells[3].h, h, 1e-9);
	CHECK_NEAR(cells[4].x, 2 + (47.0 + 2), 1e-9);
	CHECK_NEAR(cells[4].y, cells[3].y, 1e-9);
	CHECK_NEAR(cells[4].w, 47.0, 1e-9);
	CHECK_NEAR(cells[4].h, h, 1e-9);
	CHECK(cells[3].w > cells[0].w + 1);

	/* Counts per row: rows share a y band, three then two. */
	CHECK(cells[0].y == cells[1].y && cells[1].y == cells[2].y);
	CHECK(cells[3].y == cells[4].y);
	CHECK(cells[2].y != cells[3].y);

	/* No pair of cells overlaps... */
	for (int i = 0; i < 5; i++) {
		for (int j = i + 1; j < 5; j++) {
			CHECK(!rect_intersects(cells[i], cells[j]));
		}
	}
	/* ...and every cell sits inside the area with the gap respected. */
	for (int i = 0; i < 5; i++) {
		CHECK(cells[i].x >= 2);
		CHECK(cells[i].y >= 2);
		CHECK(cells[i].x + cells[i].w <= 98 + 1e-9);
		CHECK(cells[i].y + cells[i].h <= 58 + 1e-9);
	}
	/* Horizontal neighbours are exactly one gap apart. */
	CHECK_NEAR(cells[1].x - cells[0].x - cells[0].w, 2, 1e-9);
	CHECK_NEAR(cells[3].y - cells[0].y - cells[0].h, 2, 1e-9);

	/* A single cell fills the area minus the gap on every side. */
	struct rect one[1];
	tile_layout(area, 1, 2, one);
	CHECK_NEAR(one[0].x, 2, 1e-9);
	CHECK_NEAR(one[0].y, 2, 1e-9);
	CHECK_NEAR(one[0].w, 96, 1e-9);
	CHECK_NEAR(one[0].h, 56, 1e-9);

	/* Perfect square count: 2x2, no widening anywhere. */
	struct rect four[4];
	tile_layout(area, 4, 2, four);
	for (int i = 0; i < 4; i++) {
		CHECK_NEAR(four[i].w, 47.0, 1e-9);
		CHECK_NEAR(four[i].h, 27.0, 1e-9);
	}
	CHECK_NEAR(four[0].x, 2, 1e-9);
	CHECK_NEAR(four[1].x, 51, 1e-9);
	CHECK_NEAR(four[2].x, 2, 1e-9);
	CHECK_NEAR(four[3].x, 51, 1e-9);
	CHECK_NEAR(four[0].y, 2, 1e-9);
	CHECK_NEAR(four[2].y, 31, 1e-9);
}

/* ---- tile_order ---- */

static void test_tile_order(void) {
	/* Centres: (150,50) (10,50) (100,150) (20,260) (200,50); band 100 gives
	 * rows 0, 0, 1, 2, 0. Expected: row 0 by x (10, 150, 200), then rows
	 * 1 and 2 top to bottom. */
	struct rect frames[5] = {
		{145, 45, 10, 10},
		{5, 45, 10, 10},
		{95, 145, 10, 10},
		{15, 255, 10, 10},
		{195, 45, 10, 10},
	};
	size_t order[5];
	tile_order(frames, 5, 100, order);
	CHECK(order[0] == 1);
	CHECK(order[1] == 0);
	CHECK(order[2] == 4);
	CHECK(order[3] == 2);
	CHECK(order[4] == 3);

	/* Same frames shuffled on input: banding still groups rows first. */
	struct rect shuffled[5] = {
		{95, 145, 10, 10},  /* row 1 */
		{195, 45, 10, 10},  /* row 0, x 200 */
		{145, 45, 10, 10},  /* row 0, x 150 */
		{15, 255, 10, 10},  /* row 2 */
		{5, 45, 10, 10},    /* row 0, x 10 */
	};
	tile_order(shuffled, 5, 100, order);
	CHECK(order[0] == 4);
	CHECK(order[1] == 2);
	CHECK(order[2] == 1);
	CHECK(order[3] == 0);
	CHECK(order[4] == 3);

	/* Two frames in the same row keep left-to-right order; equal centres
	 * are stable (earlier index first). */
	struct rect pair[2] = {{30, 0, 10, 10}, {0, 5, 10, 10}};
	tile_order(pair, 2, 100, order);
	CHECK(order[0] == 1);
	CHECK(order[1] == 0);
}

/* ---- camera_zoom_path ---- */

static void test_camera_zoom_path(void) {
	struct camera near = {.cx = 100, .cy = 200, .zoom = 1};
	struct camera far = {.cx = 500, .cy = 300, .zoom = 0.25};

	/* Endpoints: t = 0 is exactly `near`, t = 1 is `far`. */
	struct camera c0 = camera_zoom_path(near, far, 0);
	CHECK(c0.cx == 100);
	CHECK(c0.cy == 200);
	CHECK(c0.zoom == 1);
	struct camera c1 = camera_zoom_path(near, far, 1);
	CHECK_NEAR(c1.cx, 500, 1e-9);
	CHECK_NEAR(c1.cy, 300, 1e-9);
	CHECK_NEAR(c1.zoom, 0.25, 1e-9);

	/* Zoom moves geometrically: strictly decreasing, inside the endpoints,
	 * and the geometric mean at the halfway point. */
	double prev = near.zoom;
	for (int i = 1; i < 20; i++) {
		struct camera c = camera_zoom_path(near, far, i / 20.0);
		CHECK(c.zoom < prev);
		CHECK(c.zoom > 0.25);
		prev = c.zoom;
	}
	struct camera mid = camera_zoom_path(near, far, 0.5);
	CHECK_NEAR(mid.zoom, 0.5, 1e-9);
	/* Near's centre slides toward its place in `far`: at t = 1 the screen
	 * centre lands exactly on `far`'s centre. */
	CHECK(mid.cx > near.cx && mid.cx < far.cx);
	CHECK(mid.cy > near.cy && mid.cy < far.cy);
}

/* ---- camera_viewport / fit_zoom ---- */

static void test_camera_viewport(void) {
	struct rect vp = camera_viewport((struct camera){100, 200, 2}, 800, 600);
	CHECK(vp.x == -100);
	CHECK(vp.y == 50);
	CHECK(vp.w == 400);
	CHECK(vp.h == 300);

	vp = camera_viewport((struct camera){0, 0, 0.5}, 1000, 500);
	CHECK(vp.x == -1000);
	CHECK(vp.y == -500);
	CHECK(vp.w == 2000);
	CHECK(vp.h == 1000);

	/* Zoom 1 on a vw x vh screen shows exactly vw x vh canvas units. */
	vp = camera_viewport((struct camera){7, 9, 1}, 640, 480);
	CHECK(vp.w == 640);
	CHECK(vp.h == 480);
	CHECK(vp.x == 7 - 320);
	CHECK(vp.y == 9 - 240);
}

static void test_fit_zoom(void) {
	/* Width-limited: (800-100)/1400 = 0.5, (600-100)/1000 = 0.5. */
	CHECK(fit_zoom((struct rect){0, 0, 1400, 1000}, 800, 600, 50, 1) == 0.5);
	/* Height-limited: (600-100)/2000 = 0.25 beats (800-100)/1400 = 0.5. */
	CHECK(fit_zoom((struct rect){0, 0, 1400, 2000}, 800, 600, 50, 1) == 0.25);
	/* Both axes agree at 0.25 for 2800x2000. */
	CHECK(fit_zoom((struct rect){0, 0, 2800, 2000}, 800, 600, 50, 1) == 0.25);
	/* Small content is capped by max_zoom. */
	CHECK(fit_zoom((struct rect){0, 0, 100, 100}, 800, 600, 50, 0.3) == 0.3);
	/* Empty content never shrinks the zoom below max_zoom. */
	CHECK(fit_zoom((struct rect){0, 0, 0, 0}, 800, 600, 50, 0.4) == 0.4);
	/* Margin eats into the available area. */
	CHECK(fit_zoom((struct rect){0, 0, 700, 500}, 800, 600, 50, 1) == 1);
	CHECK(fit_zoom((struct rect){0, 0, 701, 500}, 800, 600, 50, 1) < 1);
	/* A margin larger than the viewport clamps to a 1px area. */
	CHECK(fit_zoom((struct rect){0, 0, 100, 100}, 100, 100, 80, 1) < 0.02);
}

/* ---- small helpers used by the above ---- */

static void test_basics(void) {
	CHECK(clampd(5, 0, 3) == 3);
	CHECK(clampd(-1, 0, 3) == 0);
	CHECK(clampd(2, 0, 3) == 2);
	CHECK(clampd(2, 3, 0) == 3); /* hi < lo: the lo bound wins */

	CHECK(ease_out_cubic(0) == 0);
	CHECK(ease_out_cubic(1) == 1);
	CHECK(ease_out_cubic(0.5) == 0.875);
	CHECK(ease_out_cubic(-1) == 0);
	CHECK(ease_out_cubic(2) == 1);
	CHECK(ease_out_cubic(0.25) == 0.578125);

	CHECK(rect_contains((struct rect){0, 0, 10, 10}, 0, 0));
	CHECK(rect_contains((struct rect){0, 0, 10, 10}, 9.999, 9.999));
	CHECK(!rect_contains((struct rect){0, 0, 10, 10}, 10, 5));
	CHECK(!rect_contains((struct rect){0, 0, 10, 10}, -0.001, 5));

	CHECK(rect_overlap_area((struct rect){0, 0, 10, 10},
		(struct rect){5, 5, 10, 10}) == 25);
	CHECK(rect_overlap_area((struct rect){0, 0, 10, 10},
		(struct rect){10, 0, 10, 10}) == 0);
	CHECK(rect_overlap_area((struct rect){0, 0, 10, 10},
		(struct rect){0, 0, 4, 4}) == 16);

	struct rect u = rect_union((struct rect){0, 0, 10, 10},
		(struct rect){5, 5, 10, 10});
	CHECK(u.x == 0 && u.y == 0 && u.w == 15 && u.h == 15);

	/* camera_lerp: centres linear, zoom geometric. */
	struct camera a = {.cx = 0, .cy = 0, .zoom = 1};
	struct camera b = {.cx = 100, .cy = -40, .zoom = 0.25};
	struct camera m = camera_lerp(a, b, 0.5);
	CHECK(m.cx == 50);
	CHECK(m.cy == -20);
	CHECK_NEAR(m.zoom, 0.5, 1e-9);
	struct camera e0 = camera_lerp(a, b, 0);
	struct camera e1 = camera_lerp(a, b, 1);
	CHECK(e0.cx == 0 && e0.cy == 0 && e0.zoom == 1);
	CHECK_NEAR(e1.cx, 100, 1e-9);
	CHECK_NEAR(e1.cy, -40, 1e-9);
	CHECK_NEAR(e1.zoom, 0.25, 1e-9);
}

int main(void) {
	test_nav_pick_direction();
	test_nav_quantize_direction();
	test_tile_layout();
	test_tile_order();
	test_camera_zoom_path();
	test_camera_viewport();
	test_fit_zoom();
	test_basics();

	if (failures == 0) {
		printf("all geometry tests passed\n");
	} else {
		printf("%d geometry test failure(s)\n", failures);
	}
	return failures != 0;
}
