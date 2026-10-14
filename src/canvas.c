#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/render/wlr_texture.h>
#include "config.h"
#include "server.h"

/* Cap on views collected for tiling / combining; the rest stay untouched. */
#define BOX_VIEWS_MAX 64

/* Momentum slower than this (canvas px per ms) is indistinguishable from
 * still and ends the coast. */
#define COAST_STOP_SPEED 0.02

static struct rect box_to_rect(struct wlr_box box) {
	return (struct rect){box.x, box.y, box.width, box.height};
}

static void box_centre(struct wlr_box box, double *cx, double *cy) {
	*cx = box.x + box.width / 2.0;
	*cy = box.y + box.height / 2.0;
}

/* Screen-sized canvas box centred on (cx, cy), edges rounded to integers. */
static struct wlr_box centred_box(struct cv_output *output, double cx, double cy) {
	int w = output->width, h = output->height;
	return (struct wlr_box){
		(int)round(cx - w / 2.0), (int)round(cy - h / 2.0), w, h,
	};
}

static struct wlr_box rect_to_box(struct rect r) {
	return (struct wlr_box){
		(int)round(r.x), (int)round(r.y), (int)round(r.w), (int)round(r.h),
	};
}

/* ---- camera ---- */

void camera_stop(struct cv_output *output) {
	output->anim.kind = CV_ANIM_NONE;
	output->coasting = false;
	// Restart the tick clock so the next output_tick dt is a real interval.
	output->last_tick = now_ms();
}

void camera_set(struct cv_output *output, struct camera cam) {
	camera_stop(output);
	output->cam = cam;
	output->server->visibility_dirty = true;
	output->server->rebase_cursor = true;
	output_damage(output);
}

void camera_animate(struct cv_output *output, struct camera to) {
	camera_stop(output);
	output->anim.kind = CV_ANIM_LERP;
	output->anim.a = output->cam;
	output->anim.b = to;
	output->anim.start = now_ms();
	output->anim.duration = ANIM_MS;
	output_damage(output);
}

void camera_animate_zoom(struct cv_output *output, struct camera near, struct camera far,
		double p0, double p1) {
	camera_stop(output);
	output->anim.kind = CV_ANIM_ZOOM;
	output->anim.a = near;
	output->anim.b = far;
	output->anim.p0 = p0;
	output->anim.p1 = p1;
	output->anim.start = now_ms();
	output->anim.duration = ANIM_MS * clampd(fabs(p1 - p0), 0.4, 1);
	output_damage(output);
}

bool output_tick(struct cv_output *output, double now) {
	struct cv_server *server = output->server;
	double dt = now - output->last_tick;
	if (!(dt >= 0)) {
		dt = 0;
	}
	if (dt > 50) {
		dt = 50;
	}
	output->last_tick = now;

	struct camera cam = output->cam;
	bool busy = false;

	if (output->anim.kind != CV_ANIM_NONE) {
		double t = (now - output->anim.start) / output->anim.duration;
		if (t >= 1) {
			// Land exactly on the end camera, no float drift.
			if (output->anim.kind == CV_ANIM_LERP || output->anim.p1 >= 1) {
				cam = output->anim.b;
			} else if (output->anim.p1 <= 0) {
				cam = output->anim.a;
			} else {
				cam = camera_zoom_path(output->anim.a, output->anim.b, output->anim.p1);
			}
			output->anim.kind = CV_ANIM_NONE;
		} else {
			double e = ease_out_cubic(t);
			if (output->anim.kind == CV_ANIM_LERP) {
				cam = camera_lerp(output->anim.a, output->anim.b, e);
			} else {
				double p = output->anim.p0 + (output->anim.p1 - output->anim.p0) * e;
				cam = camera_zoom_path(output->anim.a, output->anim.b, p);
			}
			busy = true;
		}
	}

	if (output->coasting) {
		cam.cx += output->vel_x * dt;
		cam.cy += output->vel_y * dt;
		double decay = exp(-dt / INERTIA_TAU_MS);
		output->vel_x *= decay;
		output->vel_y *= decay;
		if (hypot(output->vel_x, output->vel_y) < COAST_STOP_SPEED) {
			output->coasting = false;
			output->vel_x = 0;
			output->vel_y = 0;
		} else {
			busy = true;
		}
	}

	double alpha_target = output_is_panning(output) ? 1.0 : 0.0;
	double alpha = output->frames_alpha;
	if (alpha < alpha_target) {
		alpha = fmin(alpha_target, alpha + dt / 150.0);
	} else if (alpha > alpha_target) {
		alpha = fmax(alpha_target, alpha - dt / 150.0);
	}

	bool cam_changed = cam.cx != output->cam.cx || cam.cy != output->cam.cy
		|| cam.zoom != output->cam.zoom;
	bool alpha_changed = alpha != output->frames_alpha;

	output->cam = cam;
	output->frames_alpha = alpha;

	if (cam_changed) {
		output->dirty = true;
		server->visibility_dirty = true;
		server->rebase_cursor = true;
	}
	if (cam_changed || alpha_changed || busy) {
		output_damage(output);
	}

	return busy || alpha != alpha_target;
}

struct camera camera_for_box(struct wlr_box box) {
	struct camera cam = {
		.cx = box.x + box.width / 2.0,
		.cy = box.y + box.height / 2.0,
		.zoom = 1,
	};
	return cam;
}

bool output_is_panning(struct cv_output *output) {
	if (output->mode != CV_MODE_NORMAL) {
		return false;
	}
	if (output->coasting || output->anim.kind == CV_ANIM_LERP) {
		return true;
	}
	struct cv_server *server = output->server;
	if (server->gesture.kind == CV_GESTURE_PAN && server->gesture.output == output) {
		return true;
	}
	if (server->cursor_mode == CV_CURSOR_PAN && server->grab_output == output) {
		return true;
	}
	return false;
}

bool output_interactive(struct cv_output *output) {
	if (output->mode != CV_MODE_NORMAL) {
		return false;
	}
	if (output->cam.zoom != 1) {
		return false;
	}
	if (output->anim.kind == CV_ANIM_ZOOM) {
		return false;
	}
	struct cv_server *server = output->server;
	if (server->gesture.output == output) {
		switch (server->gesture.kind) {
		case CV_GESTURE_ZOOM_OUT:
		case CV_GESTURE_ZOOM_IN:
		case CV_GESTURE_ZOOM_FURTHER:
		case CV_GESTURE_OVERVIEW_ZOOM:
			return false;
		default:
			break;
		}
	}
	return true;
}

/* ---- desktops ---- */

struct cv_desktop *desktop_create(struct cv_server *server, struct wlr_box box) {
	struct cv_desktop *desktop = calloc(1, sizeof(*desktop));
	if (desktop == NULL) {
		return NULL;
	}
	int id = 1;
	while (desktop_by_id(server, id) != NULL) {
		id++;
	}
	desktop->server = server;
	desktop->id = id;
	desktop->box = box;
	wl_list_insert(&server->desktops, &desktop->link);
	server_damage_all(server);
	return desktop;
}

void desktop_destroy(struct cv_desktop *desktop) {
	struct cv_server *server = desktop->server;
	struct cv_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->target_desktop == desktop) {
			output->target_desktop = NULL;
		}
	}
	if (server->grab_desktop == desktop) {
		input_cancel_grab(server);
		server->grab_desktop = NULL;
	}
	if (server->drop_desktop == desktop) {
		server->drop_desktop = NULL;
	}
	if (server->gesture.desktop == desktop) {
		server->gesture.desktop = NULL;
		// An abandon swipe has nothing left to slide; tile and combine fall back to the target.
		if (server->gesture.kind == CV_GESTURE_ABANDON) {
			server->gesture.kind = CV_GESTURE_IGNORED;
		}
	}
	if (server->gesture.merge_from == desktop) {
		server->gesture.merge_from = NULL;
	}
	if (desktop->label != NULL) {
		wlr_texture_destroy(desktop->label);
	}
	wl_list_remove(&desktop->link);
	free(desktop);
	server_damage_all(server);
}

struct cv_desktop *desktop_at(struct cv_server *server, double cx, double cy) {
	struct cv_desktop *desktop;
	wl_list_for_each(desktop, &server->desktops, link) {
		if (wlr_box_contains_point(&desktop->box, cx, cy)) {
			return desktop;
		}
	}
	return NULL;
}

struct cv_desktop *desktop_of_view(struct cv_server *server, struct cv_view *view) {
	if (view == NULL) {
		return NULL;
	}
	double cx, cy;
	box_centre(view_frame_box(view), &cx, &cy);
	return desktop_at(server, cx, cy);
}

struct cv_desktop *desktop_by_id(struct cv_server *server, int id) {
	struct cv_desktop *desktop;
	wl_list_for_each(desktop, &server->desktops, link) {
		if (desktop->id == id) {
			return desktop;
		}
	}
	return NULL;
}

struct cv_desktop *desktop_best_overlap(struct cv_server *server, struct rect area) {
	struct cv_desktop *best = NULL;
	double best_overlap = area.w * area.h * 0.5;
	struct cv_desktop *desktop;
	wl_list_for_each(desktop, &server->desktops, link) {
		double overlap = rect_overlap_area(box_to_rect(desktop->box), area);
		if (overlap < area.w * area.h * 0.5) {
			continue;
		}
		if (best == NULL || overlap > best_overlap) {
			best = desktop;
			best_overlap = overlap;
		}
	}
	return best;
}

struct cv_desktop *desktop_showing(struct cv_output *output) {
	struct camera cam = output->mode == CV_MODE_OVERVIEW ? output->home : output->cam;
	struct rect viewport = camera_viewport(cam, output->width, output->height);
	return desktop_best_overlap(output->server, viewport);
}

struct cv_desktop *desktop_nearest(struct cv_server *server, struct cv_desktop *to) {
	if (to == NULL) {
		return NULL;
	}
	double ox, oy;
	box_centre(to->box, &ox, &oy);
	struct cv_desktop *best = NULL;
	double best_dist = INFINITY;
	struct cv_desktop *desktop;
	wl_list_for_each(desktop, &server->desktops, link) {
		if (desktop == to) {
			continue;
		}
		double cx, cy;
		box_centre(desktop->box, &cx, &cy);
		double dx = cx - ox, dy = cy - oy;
		double dist = dx * dx + dy * dy;
		if (dist < best_dist) {
			best_dist = dist;
			best = desktop;
		}
	}
	return best;
}

void desktop_raise(struct cv_desktop *desktop) {
	wl_list_remove(&desktop->link);
	wl_list_insert(&desktop->server->desktops, &desktop->link);
}

/* ---- window actions ---- */

size_t views_in_box(struct cv_server *server, struct wlr_box box,
		struct cv_view **out, size_t max) {
	size_t n = 0;
	struct cv_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (n == max) {
			break;
		}
		if (!view->mapped || view->minimized) {
			continue;
		}
		double cx, cy;
		box_centre(view_frame_box(view), &cx, &cy);
		if (!wlr_box_contains_point(&box, cx, cy)) {
			continue;
		}
		out[n++] = view;
	}
	return n;
}

void canvas_tile(struct cv_server *server, struct wlr_box area, struct cv_output *output) {
	struct cv_view *views[BOX_VIEWS_MAX];
	struct rect frames[BOX_VIEWS_MAX];
	struct rect cells[BOX_VIEWS_MAX];
	size_t order[BOX_VIEWS_MAX];
	size_t n = views_in_box(server, area, views, BOX_VIEWS_MAX);
	if (n == 0) {
		hint_set(server, "No windows to tile here");
		return;
	}
	struct rect usable = box_to_rect(output_usable_canvas_box(output, area));
	size_t cols = (size_t)ceil(sqrt((double)n));
	size_t rows = (n + cols - 1) / cols;
	double band = usable.h / (double)rows;
	for (size_t i = 0; i < n; i++) {
		frames[i] = view_frame_rect(views[i]);
	}
	tile_order(frames, n, band, order);
	tile_layout(usable, n, TILE_GAP, cells);
	for (size_t i = 0; i < n; i++) {
		struct wlr_box cell = {
			(int)round(cells[i].x), (int)round(cells[i].y),
			(int)round(cells[i].w), (int)round(cells[i].h),
		};
		view_set_frame(views[order[i]], cell, true);
	}
	char text[64];
	snprintf(text, sizeof(text), "Tiled %zu window%s", n, n == 1 ? "" : "s");
	hint_set(server, text);
}

void canvas_combine(struct cv_server *server, struct cv_desktop *into,
		struct cv_desktop *from, struct wlr_box from_box) {
	if (into == NULL || from == NULL || into == from) {
		return;
	}
	int dx = into->box.x - from_box.x;
	int dy = into->box.y - from_box.y;
	struct cv_view *views[BOX_VIEWS_MAX];
	size_t n = views_in_box(server, from_box, views, BOX_VIEWS_MAX);
	for (size_t i = 0; i < n; i++) {
		struct cv_view *view = views[i];
		view_move(view, view->x + dx, view->y + dy);
		view->saved.x += dx;
		view->saved.y += dy;
	}
	int from_id = from->id, into_id = into->id;
	desktop_destroy(from);
	char text[64];
	snprintf(text, sizeof(text), "Combined Desktop %d into Desktop %d", from_id, into_id);
	hint_set(server, text);
}

/* ---- navigation ---- */

void canvas_pan(struct cv_output *output, double dx, double dy) {
	struct camera cam = output->cam;
	cam.cx += dx;
	cam.cy += dy;
	camera_set(output, cam);
}

void canvas_goto_box(struct cv_output *output, struct wlr_box box) {
	if (output->mode == CV_MODE_OVERVIEW) {
		double cx, cy;
		box_centre(box, &cx, &cy);
		overview_set_target(output, box, desktop_at(output->server, cx, cy), false);
		overview_exit(output);
		return;
	}
	camera_animate(output, camera_for_box(box));
}

void canvas_goto_direction(struct cv_output *output, double dx, double dy) {
	if (output->mode != CV_MODE_NORMAL) {
		return;
	}
	struct cv_server *server = output->server;
	struct cv_desktop *found = NULL;
	size_t n = (size_t)wl_list_length(&server->desktops);
	if (n > 0) {
		struct rect *cands = malloc(n * sizeof(*cands));
		struct cv_desktop **desktops = malloc(n * sizeof(*desktops));
		if (cands != NULL && desktops != NULL) {
			struct cv_desktop *showing = desktop_showing(output);
			int exclude = -1;
			size_t i = 0;
			struct cv_desktop *desktop;
			wl_list_for_each(desktop, &server->desktops, link) {
				cands[i] = box_to_rect(desktop->box);
				desktops[i] = desktop;
				if (desktop == showing) {
					exclude = (int)i;
				}
				i++;
			}
			int best = nav_pick_direction(output->cam.cx, output->cam.cy,
				dx, dy, cands, n, exclude, HOP_CONE_DEG);
			if (best >= 0) {
				found = desktops[best];
			}
		}
		free(cands);
		free(desktops);
	}
	if (found != NULL) {
		camera_animate(output, camera_for_box(found->box));
		return;
	}
	int qx, qy;
	nav_quantize_direction(dx, dy, &qx, &qy);
	struct camera to = output->cam;
	to.cx += qx * (double)output->width;
	to.cy += qy * (double)output->height;
	to.zoom = 1;
	camera_animate(output, to);
}

void canvas_reveal_view(struct cv_server *server, struct cv_view *view) {
	if (view == NULL) {
		return;
	}
	struct cv_output *output = output_under_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box frame = view_frame_box(view);
	double cx, cy;
	box_centre(frame, &cx, &cy);
	if (output->mode == CV_MODE_NORMAL) {
		struct rect vp = output_viewport(output);
		if (frame.x >= vp.x && frame.y >= vp.y
				&& frame.x + frame.width <= vp.x + vp.w
				&& frame.y + frame.height <= vp.y + vp.h) {
			return;
		}
	}
	struct cv_desktop *desktop = desktop_of_view(server, view);
	struct wlr_box box = desktop != NULL ? desktop->box : centred_box(output, cx, cy);
	if (output->mode == CV_MODE_OVERVIEW) {
		overview_set_target(output, box, desktop, false);
		overview_exit(output);
		return;
	}
	camera_animate(output, camera_for_box(box));
}

void canvas_pin_desktop(struct cv_output *output) {
	struct cv_server *server = output->server;
	if (output->mode == CV_MODE_OVERVIEW) {
		if (output->target_desktop != NULL) {
			hint_set(server, "Already a desktop");
			return;
		}
		struct cv_desktop *desktop = desktop_create(server, output->target);
		if (desktop != NULL) {
			output->target_desktop = desktop;
			char text[32];
			snprintf(text, sizeof(text), "New Desktop %d", desktop->id);
			hint_set(server, text);
		}
		return;
	}
	struct rect viewport = output_viewport(output);
	struct cv_desktop *desktop = desktop_best_overlap(server, viewport);
	if (desktop != NULL
			&& rect_overlap_area(box_to_rect(desktop->box), viewport)
				>= 0.9 * viewport.w * viewport.h) {
		hint_set(server, "Already a desktop");
		return;
	}
	desktop = desktop_create(server, rect_to_box(viewport));
	if (desktop != NULL) {
		char text[32];
		snprintf(text, sizeof(text), "New Desktop %d", desktop->id);
		hint_set(server, text);
	}
}

/* ---- overview ---- */

struct camera overview_camera(struct cv_output *output, struct camera home) {
	double vw = output->width, vh = output->height;
	struct rect content = camera_viewport(home, vw, vh);
	struct cv_desktop *desktop;
	wl_list_for_each(desktop, &output->server->desktops, link) {
		content = rect_union(content, box_to_rect(desktop->box));
	}
	// Every window on the canvas, so the overview shows what's out there.
	struct cv_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->mapped && !view->minimized) {
			content = rect_union(content, view_frame_rect(view));
		}
	}
	// Symmetric content around the home centre keeps the home screen steady
	// when it sits off-centre inside the content.
	double ex = fmax(home.cx - content.x, content.x + content.w - home.cx);
	double ey = fmax(home.cy - content.y, content.y + content.h - home.cy);
	struct rect sym = {home.cx - ex, home.cy - ey, 2 * ex, 2 * ey};
	double zoom_c = fit_zoom(sym, vw, vh, OVERVIEW_MARGIN, OVERVIEW_MAX_ZOOM);
	double zoom_f = fit_zoom(content, vw, vh, OVERVIEW_MARGIN, OVERVIEW_MAX_ZOOM);
	if (zoom_c >= 0.75 * zoom_f) {
		return (struct camera){.cx = home.cx, .cy = home.cy,
			.zoom = fmax(zoom_c, OVERVIEW_MIN_ZOOM)};
	}
	return (struct camera){.cx = rect_cx(content), .cy = rect_cy(content),
		.zoom = fmax(zoom_f, OVERVIEW_MIN_ZOOM)};
}

struct camera overview_prepare(struct cv_output *output) {
	camera_stop(output);
	struct camera home = output->cam;
	home.zoom = 1;
	output->home = home;
	struct cv_desktop *desktop = desktop_showing(output);
	if (desktop != NULL) {
		output->target = desktop->box;
		output->target_desktop = desktop;
	} else {
		output->target = rect_to_box(camera_viewport(home, output->width, output->height));
		output->target_desktop = NULL;
	}
	struct camera far = overview_camera(output, home);
	output->overview_zoom = far.zoom;
	return far;
}

void overview_enter(struct cv_output *output) {
	if (output->mode == CV_MODE_OVERVIEW) {
		return;
	}
	struct camera far = overview_prepare(output);
	overview_finish_enter(output, far, 0);
}

void overview_finish_enter(struct cv_output *output, struct camera far, double t) {
	struct cv_server *server = output->server;
	input_cancel_grab(server);
	output->mode = CV_MODE_OVERVIEW;
	wlr_seat_pointer_clear_focus(server->seat);
	camera_animate_zoom(output, output->home, far, t, 1);
	output_damage(output);
}

void overview_exit(struct cv_output *output) {
	struct camera near = camera_for_box(output->target);
	overview_finish_exit(output, near, output->cam, 1);
}

void overview_finish_exit(struct cv_output *output, struct camera near,
		struct camera far, double t) {
	struct cv_server *server = output->server;
	input_cancel_grab(server);
	output->mode = CV_MODE_NORMAL;
	camera_animate_zoom(output, near, far, t, 0);
	// Keyboard focus follows the target: if the focused window is not in it,
	// focus the topmost one that is.
	struct cv_view *focused = server->focused_view;
	bool inside = false;
	if (focused != NULL) {
		double cx, cy;
		box_centre(view_frame_box(focused), &cx, &cy);
		inside = wlr_box_contains_point(&output->target, cx, cy);
	}
	if (!inside) {
		struct cv_view *view;
		if (views_in_box(server, output->target, &view, 1) > 0) {
			view_focus(view);
		}
	}
	server->rebase_cursor = true;
}

void overview_cancel(struct cv_output *output) {
	if (output->mode != CV_MODE_OVERVIEW) {
		return;
	}
	struct cv_desktop *desktop = desktop_showing(output);
	if (desktop != NULL) {
		output->target = desktop->box;
		output->target_desktop = desktop;
	} else {
		output->target = rect_to_box(
			camera_viewport(output->home, output->width, output->height));
		output->target_desktop = NULL;
	}
	overview_finish_exit(output, output->home, output->cam, 1);
}

void overview_hop(struct cv_output *output, double dx, double dy) {
	struct cv_server *server = output->server;
	double ox, oy;
	box_centre(output->target, &ox, &oy);
	struct cv_desktop *found = NULL;
	size_t n = (size_t)wl_list_length(&server->desktops);
	if (n > 0) {
		struct rect *cands = malloc(n * sizeof(*cands));
		struct cv_desktop **desktops = malloc(n * sizeof(*desktops));
		if (cands != NULL && desktops != NULL) {
			int exclude = -1;
			size_t i = 0;
			struct cv_desktop *desktop;
			wl_list_for_each(desktop, &server->desktops, link) {
				cands[i] = box_to_rect(desktop->box);
				desktops[i] = desktop;
				if (desktop == output->target_desktop) {
					exclude = (int)i;
				}
				i++;
			}
			int best = nav_pick_direction(ox, oy, dx, dy,
				cands, n, exclude, HOP_CONE_DEG);
			if (best >= 0) {
				found = desktops[best];
			}
		}
		free(cands);
		free(desktops);
	}
	if (found != NULL) {
		overview_set_target(output, found->box, found, true);
		return;
	}
	int qx, qy;
	nav_quantize_direction(dx, dy, &qx, &qy);
	double cx = ox + qx * (double)output->width;
	double cy = oy + qy * (double)output->height;
	overview_set_target(output, target_box_at(output, cx, cy),
		desktop_at(server, cx, cy), true);
}

void overview_set_target(struct cv_output *output, struct wlr_box box,
		struct cv_desktop *desktop, bool center) {
	output->target = box;
	output->target_desktop = desktop;
	output_damage(output);
	if (center) {
		struct camera cam = output->cam;
		box_centre(box, &cam.cx, &cam.cy);
		camera_animate(output, cam);
	}
}

void overview_target_point(struct cv_output *output, double cx, double cy) {
	overview_set_target(output, target_box_at(output, cx, cy),
		desktop_at(output->server, cx, cy), false);
}

void overview_zoom_by(struct cv_output *output, double factor, double ox, double oy) {
	double zoom = clampd(output->cam.zoom * factor, OVERVIEW_MIN_ZOOM, 0.75);
	double cx, cy;
	output_local_to_canvas(output, ox, oy, &cx, &cy);
	struct camera cam = {
		.cx = cx - (ox - output->width / 2.0) / zoom,
		.cy = cy - (oy - output->height / 2.0) / zoom,
		.zoom = zoom,
	};
	camera_set(output, cam);
	output->overview_zoom = zoom;
}

void overview_abandon(struct cv_output *output, struct cv_desktop *desktop) {
	if (desktop == NULL) {
		hint_set(output->server, "Nothing to abandon here");
		return;
	}
	int id = desktop->id;
	// desktop_destroy clears target_desktop if it was this one; the target
	// box stays put and becomes a free spot.
	desktop_destroy(desktop);
	char text[64];
	snprintf(text, sizeof(text), "Abandoned Desktop %d — its windows stay on the canvas", id);
	hint_set(output->server, text);
}

void overview_tile(struct cv_output *output, struct cv_desktop *desktop) {
	canvas_tile(output->server, desktop != NULL ? desktop->box : output->target, output);
}

void overview_combine(struct cv_output *output, struct cv_desktop *into) {
	struct cv_server *server = output->server;
	if (into == NULL) {
		hint_set(server, "Nothing to combine here");
		return;
	}
	struct cv_desktop *from = desktop_nearest(server, into);
	if (from == NULL) {
		hint_set(server, "No other desktop to combine");
		return;
	}
	canvas_combine(server, into, from, from->box);
}

void overview_new_desktop(struct cv_output *output, double cx, double cy) {
	struct wlr_box box = centred_box(output, cx, cy);
	struct cv_desktop *desktop = desktop_create(output->server, box);
	if (desktop == NULL) {
		return;
	}
	overview_set_target(output, box, desktop, false);
	char text[32];
	snprintf(text, sizeof(text), "New Desktop %d", desktop->id);
	hint_set(output->server, text);
}

struct wlr_box target_box_at(struct cv_output *output, double cx, double cy) {
	struct cv_desktop *desktop = desktop_at(output->server, cx, cy);
	if (desktop != NULL) {
		return desktop->box;
	}
	return centred_box(output, cx, cy);
}
