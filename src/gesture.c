#include <math.h>
#include <stdio.h>
#include "config.h"
#include "server.h"

/* Travel (touchpad units) before a pending gesture is classified. */
#define DECIDE_THRESHOLD 12.0

/* One hop per 3-finger gesture. */
static bool g_hop_done;
/* Whether the current hold gesture was forwarded to a client. */
static bool g_hold_forwarded;
/* Whether this pinch currently shows a tile/combine preview hint. */
static bool g_pinch_hinted;

static void gesture_reset(struct cv_server *server) {
	struct cv_gesture *g = &server->gesture;
	g->kind = CV_GESTURE_NONE;
	g->pinch = false;
	g->fingers = 0;
	g->output = NULL;
	g->ax = 0;
	g->ay = 0;
	g->scale = 1;
	g->vx = 0;
	g->vy = 0;
	g->progress = 0;
	g->start_zoom = 0;
	g->desktop = NULL;
	g->merge_from = NULL;
	g_hop_done = false;
	g_pinch_hinted = false;
}

/* Touchpad delta -> canvas delta, natural pan sign, scaled by the zoom. */
static void pan_deltas(struct cv_output *output, double dx, double dy,
		double *cdx, double *cdy) {
	double sign = NATURAL_PAN ? -1.0 : 1.0;
	double zoom = output->cam.zoom > 0 ? output->cam.zoom : 1.0;
	*cdx = sign * dx * PAN_SPEED / zoom;
	*cdy = sign * dy * PAN_SPEED / zoom;
}

/* Desktop under the cursor (canvas coords); falls back to the overview
 * target desktop when the cursor is not over any desktop. */
static struct cv_desktop *gesture_desktop_at(struct cv_server *server,
		struct cv_output *output) {
	struct cv_output *cursor_output;
	double cx, cy;
	if (cursor_canvas_pos(server, &cursor_output, &cx, &cy)) {
		struct cv_desktop *desktop = desktop_at(server, cx, cy);
		if (desktop != NULL) {
			return desktop;
		}
	}
	return output->target_desktop;
}

/* Classify a pending gesture now that enough travel has accumulated. */
static void gesture_decide(struct cv_server *server) {
	struct cv_gesture *g = &server->gesture;
	struct cv_output *o = g->output;
	bool vertical = fabs(g->ay) > fabs(g->ax);

	if (o->mode == CV_MODE_NORMAL) {
		if (g->fingers == 3) {
			g->kind = CV_GESTURE_PAN;
			// Apply the travel accumulated so far in one go.
			double cdx, cdy;
			pan_deltas(o, g->ax, g->ay, &cdx, &cdy);
			canvas_pan(o, cdx, cdy);
		} else if (g->fingers == 4 && vertical && g->ay < 0) {
			g->kind = CV_GESTURE_ZOOM_OUT;
			g->far = overview_prepare(o);
			g->near = o->home;
		} else {
			g->kind = CV_GESTURE_IGNORED;
		}
		return;
	}

	/* Overview mode. */
	if (g->fingers == 3) {
		g->kind = CV_GESTURE_HOP;
	} else if (g->fingers == 4) {
		if (vertical && g->ay > 0) {
			g->kind = CV_GESTURE_ZOOM_IN;
			g->near = camera_for_box(o->target);
			g->far = o->cam;
		} else if (vertical && g->ay < 0) {
			g->kind = CV_GESTURE_ZOOM_FURTHER;
			g->start_zoom = o->cam.zoom;
		} else {
			struct cv_desktop *d = gesture_desktop_at(server, o);
			if (d == NULL) {
				g->kind = CV_GESTURE_IGNORED;
			} else {
				g->kind = CV_GESTURE_ABANDON;
				g->desktop = d;
			}
		}
	} else {
		g->kind = CV_GESTURE_IGNORED;
	}
}

void gesture_swipe_begin(struct cv_server *server, uint32_t time, uint32_t fingers) {
	struct cv_gesture *g = &server->gesture;
	struct cv_output *output = output_under_cursor(server);
	gesture_reset(server);
	if (output == NULL) {
		g->kind = CV_GESTURE_IGNORED;
		return;
	}
	g->output = output;
	g->fingers = fingers;
	g->last_time = time;
	if (fingers == 3 || fingers == 4) {
		g->kind = CV_GESTURE_PENDING;
		return;
	}
	if (output_interactive(output)) {
		g->kind = CV_GESTURE_FORWARD;
		wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures,
			server->seat, time, fingers);
		return;
	}
	g->kind = CV_GESTURE_IGNORED;
}

void gesture_swipe_update(struct cv_server *server, uint32_t time, double dx, double dy) {
	struct cv_gesture *g = &server->gesture;
	if (g->kind == CV_GESTURE_FORWARD) {
		wlr_pointer_gestures_v1_send_swipe_update(server->pointer_gestures,
			server->seat, time, dx, dy);
		return;
	}
	if (g->output == NULL || g->kind == CV_GESTURE_NONE ||
			g->kind == CV_GESTURE_IGNORED) {
		return;
	}
	struct cv_output *o = g->output;
	g->ax += dx;
	g->ay += dy;
	double dt = (double)time - (double)g->last_time;

	bool just_decided = false;
	if (g->kind == CV_GESTURE_PENDING && hypot(g->ax, g->ay) > DECIDE_THRESHOLD) {
		gesture_decide(server);
		just_decided = true;
	}

	switch (g->kind) {
	case CV_GESTURE_PAN: {
		double cdx, cdy;
		pan_deltas(o, dx, dy, &cdx, &cdy);
		if (dt > 0) {
			g->vx = 0.8 * g->vx + 0.2 * (cdx / dt);
			g->vy = 0.8 * g->vy + 0.2 * (cdy / dt);
		}
		// The deciding update already applied the accumulated travel.
		if (!just_decided) {
			canvas_pan(o, cdx, cdy);
		}
		break;
	}
	case CV_GESTURE_HOP:
		if (!g_hop_done && hypot(g->ax, g->ay) > HOP_THRESHOLD) {
			overview_hop(o, g->ax, g->ay);
			g_hop_done = true;
		}
		break;
	case CV_GESTURE_ZOOM_OUT:
		if (dt > 0) {
			g->vy = 0.8 * g->vy + 0.2 * (dy / dt);
		}
		g->progress = clampd(-g->ay / OVERVIEW_SWIPE_DISTANCE, 0, 1);
		camera_set(o, camera_zoom_path(g->near, g->far, g->progress));
		break;
	case CV_GESTURE_ZOOM_IN:
		if (dt > 0) {
			g->vy = 0.8 * g->vy + 0.2 * (dy / dt);
		}
		g->progress = clampd(g->ay / OVERVIEW_SWIPE_DISTANCE, 0, 1);
		camera_set(o, camera_zoom_path(g->near, g->far, 1 - g->progress));
		break;
	case CV_GESTURE_ZOOM_FURTHER: {
		if (dt > 0) {
			g->vy = 0.8 * g->vy + 0.2 * (dy / dt);
		}
		struct camera cam = o->cam;
		cam.zoom = clampd(g->start_zoom * exp(g->ay / 400),
			OVERVIEW_MIN_ZOOM, 0.75);
		camera_set(o, cam);
		break;
	}
	case CV_GESTURE_ABANDON:
		g->desktop->swipe_offset = g->ax;
		output_damage(o);
		break;
	default:
		break;
	}
	g->last_time = time;
}

void gesture_swipe_end(struct cv_server *server, uint32_t time, bool cancelled) {
	struct cv_gesture *g = &server->gesture;
	if (g->kind == CV_GESTURE_FORWARD) {
		wlr_pointer_gestures_v1_send_swipe_end(server->pointer_gestures,
			server->seat, time, cancelled);
		gesture_reset(server);
		return;
	}
	struct cv_output *o = g->output;
	if (o != NULL) {
		switch (g->kind) {
		case CV_GESTURE_PAN: {
			// Where the momentum would leave the camera.
			struct camera cam = output_camera(o);
			double lx = cam.cx + g->vx * INERTIA_TAU_MS;
			double ly = cam.cy + g->vy * INERTIA_TAU_MS;
			double snap = SNAP_RADIUS * fmin(o->width, o->height);
			struct cv_desktop *best = NULL;
			double best_dist = HUGE_VAL;
			struct cv_desktop *d;
			wl_list_for_each(d, &server->desktops, link) {
				double dcx = d->box.x + d->box.width / 2.0;
				double dcy = d->box.y + d->box.height / 2.0;
				double dist = hypot(lx - dcx, ly - dcy);
				if (dist < best_dist) {
					best_dist = dist;
					best = d;
				}
			}
			if (best != NULL && best_dist <= snap) {
				camera_animate(o, camera_for_box(best->box));
			} else if (!cancelled && hypot(g->vx, g->vy) > INERTIA_MIN_SPEED) {
				o->vel_x = g->vx;
				o->vel_y = g->vy;
				o->coasting = true;
				o->last_tick = now_ms();
				output_damage(o);
			}
			break;
		}
		case CV_GESTURE_ZOOM_OUT:
			if (!cancelled && (g->progress > OVERVIEW_COMMIT || g->vy < -0.5)) {
				overview_finish_enter(o, g->far, g->progress);
			} else {
				camera_animate_zoom(o, g->near, g->far, g->progress, 0);
			}
			break;
		case CV_GESTURE_ZOOM_IN:
			if (!cancelled && (g->progress > OVERVIEW_COMMIT || g->vy > 0.5)) {
				overview_finish_exit(o, g->near, g->far, 1 - g->progress);
			} else {
				camera_animate_zoom(o, g->near, g->far, 1 - g->progress, 1);
			}
			break;
		case CV_GESTURE_ZOOM_FURTHER:
			o->overview_zoom = o->cam.zoom;
			break;
		case CV_GESTURE_ABANDON:
			if (g->desktop != NULL) {
				if (fabs(g->desktop->swipe_offset) > ABANDON_DISTANCE && !cancelled) {
					overview_abandon(o, g->desktop);
				} else {
					g->desktop->swipe_offset = 0;
					output_damage(o);
				}
			}
			break;
		default:
			break;
		}
		output_damage(o);
	}
	gesture_reset(server);
}

void gesture_pinch_begin(struct cv_server *server, uint32_t time, uint32_t fingers) {
	struct cv_gesture *g = &server->gesture;
	struct cv_output *output = output_under_cursor(server);
	gesture_reset(server);
	if (output == NULL) {
		g->kind = CV_GESTURE_IGNORED;
		return;
	}
	g->output = output;
	g->fingers = fingers;
	g->last_time = time;

	if (output->mode == CV_MODE_OVERVIEW) {
		switch (fingers) {
		case 2:
			g->kind = CV_GESTURE_OVERVIEW_ZOOM;
			return;
		case 3:
			g->kind = CV_GESTURE_PINCH;
			// NULL target is allowed: tile then acts on the target box.
			g->desktop = gesture_desktop_at(server, output);
			return;
		default:
			if (fingers >= 4) {
				// Travel drives the 4-finger swipe logic.
				g->kind = CV_GESTURE_PENDING;
				g->pinch = true;
			} else {
				g->kind = CV_GESTURE_IGNORED;
			}
			return;
		}
	}

	/* Normal mode: sloppy 3/4-finger swipes arrive as pinches; treat the
	 * travel as a swipe so they still pan / open the overview. */
	if (fingers >= 3) {
		g->kind = CV_GESTURE_PENDING;
		g->pinch = true;
		return;
	}
	if (fingers == 2 && output_interactive(output)) {
		g->kind = CV_GESTURE_FORWARD;
		wlr_pointer_gestures_v1_send_pinch_begin(server->pointer_gestures,
			server->seat, time, fingers);
		return;
	}
	g->kind = CV_GESTURE_IGNORED;
}

static void pinch_preview(struct cv_server *server, const char *text) {
	hint_set(server, text);
	g_pinch_hinted = true;
}

static void pinch_clear_preview(struct cv_server *server, bool clear_merge) {
	if (clear_merge) {
		server->gesture.merge_from = NULL;
	}
	if (g_pinch_hinted) {
		hint_set(server, NULL);
		g_pinch_hinted = false;
	}
}

void gesture_pinch_update(struct cv_server *server, uint32_t time, double dx, double dy,
		double scale, double rotation) {
	struct cv_gesture *g = &server->gesture;
	if (g->kind == CV_GESTURE_FORWARD) {
		wlr_pointer_gestures_v1_send_pinch_update(server->pointer_gestures,
			server->seat, time, dx, dy, scale, rotation);
		return;
	}
	if (g->output == NULL || g->kind == CV_GESTURE_NONE ||
			g->kind == CV_GESTURE_IGNORED) {
		return;
	}
	struct cv_output *o = g->output;

	switch (g->kind) {
	case CV_GESTURE_OVERVIEW_ZOOM: {
		double factor = g->scale > 0 ? scale / g->scale : 1;
		double ox = server->cursor->x - o->lx;
		double oy = server->cursor->y - o->ly;
		overview_zoom_by(o, factor, ox, oy);
		g->scale = scale;
		break;
	}
	case CV_GESTURE_PINCH:
		g->scale = scale;
		if (scale > PINCH_TILE_SCALE) {
			g->merge_from = NULL;
			if (g->desktop != NULL) {
				char buf[64];
				snprintf(buf, sizeof(buf), "Release to tile Desktop %d",
					g->desktop->id);
				pinch_preview(server, buf);
			} else {
				pinch_preview(server, "Release to tile windows here");
			}
		} else if (scale < PINCH_COMBINE_SCALE) {
			if (g->desktop == NULL) {
				g->merge_from = NULL;
				pinch_preview(server, "Pinch over a desktop to combine");
			} else {
				g->merge_from = desktop_nearest(server, g->desktop);
				if (g->merge_from != NULL) {
					char buf[80];
					snprintf(buf, sizeof(buf),
						"Release to combine Desktop %d into Desktop %d",
						g->merge_from->id, g->desktop->id);
					pinch_preview(server, buf);
				} else {
					pinch_clear_preview(server, false);
				}
			}
		} else {
			// Back in the neutral range: drop the preview.
			pinch_clear_preview(server, true);
		}
		output_damage(o);
		break;
	default:
		/* Pending / decided swipe-like gestures: treat the travel of the
		 * pinch as swipe motion. gesture_swipe_update updates last_time. */
		gesture_swipe_update(server, time, dx, dy);
		return;
	}
	g->last_time = time;
}

void gesture_pinch_end(struct cv_server *server, uint32_t time, bool cancelled) {
	struct cv_gesture *g = &server->gesture;
	if (g->kind == CV_GESTURE_FORWARD) {
		wlr_pointer_gestures_v1_send_pinch_end(server->pointer_gestures,
			server->seat, time, cancelled);
		gesture_reset(server);
		return;
	}
	if (g->kind == CV_GESTURE_PINCH) {
		struct cv_output *o = g->output;
		if (o != NULL) {
			if (!cancelled && g->scale > PINCH_TILE_SCALE) {
				overview_tile(o, g->desktop);
			} else if (!cancelled && g->merge_from != NULL) {
				overview_combine(o, g->desktop);
			}
			output_damage(o);
		}
		pinch_clear_preview(server, true);
		gesture_reset(server);
		return;
	}
	/* Everything else (pending, pan, zoom, hop, abandon) releases like a
	 * swipe; gesture_swipe_end resets the gesture state. */
	gesture_swipe_end(server, time, cancelled);
}

void gesture_hold_begin(struct cv_server *server, uint32_t time, uint32_t fingers) {
	g_hold_forwarded = false;
	struct cv_output *output = output_under_cursor(server);
	if (output == NULL) {
		return;
	}
	// Touching the pad halts any coasting momentum.
	output->coasting = false;
	output->vel_x = 0;
	output->vel_y = 0;
	if (output_interactive(output)) {
		wlr_pointer_gestures_v1_send_hold_begin(server->pointer_gestures,
			server->seat, time, fingers);
		g_hold_forwarded = true;
	}
}

void gesture_hold_end(struct cv_server *server, uint32_t time, bool cancelled) {
	if (g_hold_forwarded) {
		wlr_pointer_gestures_v1_send_hold_end(server->pointer_gestures,
			server->seat, time, cancelled);
		g_hold_forwarded = false;
	}
}
