#include <cmath>
#include <cstdio>

#include "config.h"
#include "desktop.h"
#include "output.h"
#include "server.h"

namespace {

// Travel (touchpad units) before a pending gesture is classified.
constexpr double decide_threshold = 12.0;

// Touchpad delta -> canvas delta, natural pan sign, scaled by the zoom.
Point pan_deltas(const Output &output, double dx, double dy) {
	double sign = config::natural_pan ? -1.0 : 1.0;
	double zoom = output.cam.zoom > 0 ? output.cam.zoom : 1.0;
	return {sign * dx * config::pan_speed / zoom, sign * dy * config::pan_speed / zoom};
}

} // namespace

Gesture::Gesture(Server &server)
	: server_(server) {
}

void Gesture::reset() {
	kind = GestureKind::none;
	pinch = false;
	fingers = 0;
	output = nullptr;
	ax = 0;
	ay = 0;
	scale = 1;
	vx = 0;
	vy = 0;
	progress = 0;
	start_zoom = 0;
	desktop = nullptr;
	merge_from = nullptr;
	hop_done_ = false;
	pinch_hinted_ = false;
}

Desktop *Gesture::desktop_under(Output &out) {
	if (std::optional<CanvasPos> pos = server_.input.cursor_canvas_pos()) {
		if (Desktop *desktop = server_.desktop_at(pos->cx, pos->cy)) {
			return desktop;
		}
	}
	return out.target_desktop;
}

void Gesture::decide() {
	Output *o = output;
	bool vertical = std::fabs(ay) > std::fabs(ax);

	if (o->mode == Mode::normal) {
		if (fingers == 3) {
			kind = GestureKind::pan;
			// Apply the travel accumulated so far in one go.
			Point d = pan_deltas(*o, ax, ay);
			o->pan(d.x, d.y);
		} else if (fingers == 4 && vertical && ay < 0) {
			kind = GestureKind::zoom_out;
			far = o->overview_prepare();
			near = o->home;
		} else {
			kind = GestureKind::ignored;
		}
		return;
	}

	// Overview mode.
	if (fingers == 3) {
		kind = GestureKind::hop;
	} else if (fingers == 4) {
		if (vertical && ay > 0) {
			kind = GestureKind::zoom_in;
			near = camera_for_box(o->target);
			far = o->cam;
		} else if (vertical && ay < 0) {
			kind = GestureKind::zoom_further;
			start_zoom = o->cam.zoom;
		} else {
			Desktop *d = desktop_under(*o);
			if (d == nullptr) {
				kind = GestureKind::ignored;
			} else {
				kind = GestureKind::abandon;
				desktop = d;
			}
		}
	} else {
		kind = GestureKind::ignored;
	}
}

void Gesture::swipe_begin(uint32_t time, uint32_t fingers) {
	Output *out = server_.output_under_cursor();
	reset();
	if (out == nullptr) {
		kind = GestureKind::ignored;
		return;
	}
	output = out;
	this->fingers = fingers;
	last_time = time;
	if (fingers == 3 || fingers == 4) {
		kind = GestureKind::pending;
		return;
	}
	if (out->interactive()) {
		kind = GestureKind::forward;
		wlr_pointer_gestures_v1_send_swipe_begin(server_.pointer_gestures,
			server_.seat, time, fingers);
		return;
	}
	kind = GestureKind::ignored;
}

void Gesture::swipe_update(uint32_t time, double dx, double dy) {
	if (kind == GestureKind::forward) {
		wlr_pointer_gestures_v1_send_swipe_update(server_.pointer_gestures,
			server_.seat, time, dx, dy);
		return;
	}
	if (output == nullptr || kind == GestureKind::none ||
			kind == GestureKind::ignored) {
		return;
	}
	Output *o = output;
	ax += dx;
	ay += dy;
	double dt = static_cast<double>(time) - static_cast<double>(last_time);

	bool just_decided = false;
	if (kind == GestureKind::pending && std::hypot(ax, ay) > decide_threshold) {
		decide();
		just_decided = true;
	}

	switch (kind) {
	case GestureKind::pan: {
		Point d = pan_deltas(*o, dx, dy);
		if (dt > 0) {
			vx = 0.8 * vx + 0.2 * (d.x / dt);
			vy = 0.8 * vy + 0.2 * (d.y / dt);
		}
		// The deciding update already applied the accumulated travel.
		if (!just_decided) {
			o->pan(d.x, d.y);
		}
		break;
	}
	case GestureKind::hop:
		if (!hop_done_ && std::hypot(ax, ay) > config::hop_threshold) {
			o->overview_hop(ax, ay);
			hop_done_ = true;
		}
		break;
	case GestureKind::zoom_out:
		if (dt > 0) {
			vy = 0.8 * vy + 0.2 * (dy / dt);
		}
		progress = clampd(-ay / config::overview_swipe_distance, 0, 1);
		o->set_camera(camera_zoom_path(near, far, progress));
		break;
	case GestureKind::zoom_in:
		if (dt > 0) {
			vy = 0.8 * vy + 0.2 * (dy / dt);
		}
		progress = clampd(ay / config::overview_swipe_distance, 0, 1);
		o->set_camera(camera_zoom_path(near, far, 1 - progress));
		break;
	case GestureKind::zoom_further: {
		if (dt > 0) {
			vy = 0.8 * vy + 0.2 * (dy / dt);
		}
		Camera cam = o->cam;
		cam.zoom = clampd(start_zoom * std::exp(ay / 400),
			config::overview_min_zoom, 0.75);
		o->set_camera(cam);
		break;
	}
	case GestureKind::abandon:
		desktop->swipe_offset = ax;
		o->damage();
		break;
	default:
		break;
	}
	last_time = time;
}

void Gesture::swipe_end(uint32_t time, bool cancelled) {
	if (kind == GestureKind::forward) {
		wlr_pointer_gestures_v1_send_swipe_end(server_.pointer_gestures,
			server_.seat, time, cancelled);
		reset();
		return;
	}
	if (Output *o = output) {
		switch (kind) {
		case GestureKind::pan: {
			// Where the momentum would leave the camera.
			Camera cam = o->camera();
			double lx = cam.cx + vx * config::inertia_tau_ms;
			double ly = cam.cy + vy * config::inertia_tau_ms;
			double snap = config::snap_radius * std::fmin(o->width, o->height);
			Desktop *best = nullptr;
			double best_dist = HUGE_VAL;
			for (const auto &d : server_.desktops) {
				double dcx = d->box.x + d->box.width / 2.0;
				double dcy = d->box.y + d->box.height / 2.0;
				double dist = std::hypot(lx - dcx, ly - dcy);
				if (dist < best_dist) {
					best_dist = dist;
					best = d.get();
				}
			}
			if (best != nullptr && best_dist <= snap) {
				o->animate_camera(camera_for_box(best->box));
			} else if (!cancelled && std::hypot(vx, vy) > config::inertia_min_speed) {
				o->vel_x = vx;
				o->vel_y = vy;
				o->coasting = true;
				o->last_tick = now_ms();
				o->damage();
			}
			break;
		}
		case GestureKind::zoom_out:
			if (!cancelled && (progress > config::overview_commit || vy < -0.5)) {
				o->overview_finish_enter(far, progress);
			} else {
				o->animate_zoom(near, far, progress, 0);
			}
			break;
		case GestureKind::zoom_in:
			if (!cancelled && (progress > config::overview_commit || vy > 0.5)) {
				o->overview_finish_exit(near, far, 1 - progress);
			} else {
				o->animate_zoom(near, far, 1 - progress, 1);
			}
			break;
		case GestureKind::zoom_further:
			o->overview_zoom = o->cam.zoom;
			break;
		case GestureKind::abandon:
			if (desktop != nullptr) {
				if (std::fabs(desktop->swipe_offset) > config::abandon_distance && !cancelled) {
					o->overview_abandon(desktop);
				} else {
					desktop->swipe_offset = 0;
					o->damage();
				}
			}
			break;
		default:
			break;
		}
		o->damage();
	}
	reset();
}

void Gesture::pinch_begin(uint32_t time, uint32_t fingers) {
	Output *out = server_.output_under_cursor();
	reset();
	if (out == nullptr) {
		kind = GestureKind::ignored;
		return;
	}
	output = out;
	this->fingers = fingers;
	last_time = time;

	if (out->mode == Mode::overview) {
		switch (fingers) {
		case 2:
			kind = GestureKind::overview_zoom;
			return;
		case 3:
			kind = GestureKind::pinch;
			// A null target is allowed: tile then acts on the target box.
			desktop = desktop_under(*out);
			return;
		default:
			if (fingers >= 4) {
				// Travel drives the 4-finger swipe logic.
				kind = GestureKind::pending;
				pinch = true;
			} else {
				kind = GestureKind::ignored;
			}
			return;
		}
	}

	// Normal mode: sloppy 3/4-finger swipes arrive as pinches; treat the
	// travel as a swipe so they still pan / open the overview.
	if (fingers >= 3) {
		kind = GestureKind::pending;
		pinch = true;
		return;
	}
	if (fingers == 2 && out->interactive()) {
		kind = GestureKind::forward;
		wlr_pointer_gestures_v1_send_pinch_begin(server_.pointer_gestures,
			server_.seat, time, fingers);
		return;
	}
	kind = GestureKind::ignored;
}

void Gesture::pinch_preview(std::string_view text) {
	server_.hint.set(text);
	pinch_hinted_ = true;
}

void Gesture::pinch_clear_preview(bool clear_merge) {
	if (clear_merge) {
		merge_from = nullptr;
	}
	if (pinch_hinted_) {
		server_.hint.set({});
		pinch_hinted_ = false;
	}
}

void Gesture::pinch_update(uint32_t time, double dx, double dy,
		double scale, double rotation) {
	if (kind == GestureKind::forward) {
		wlr_pointer_gestures_v1_send_pinch_update(server_.pointer_gestures,
			server_.seat, time, dx, dy, scale, rotation);
		return;
	}
	if (output == nullptr || kind == GestureKind::none ||
			kind == GestureKind::ignored) {
		return;
	}
	Output *o = output;

	switch (kind) {
	case GestureKind::overview_zoom: {
		double factor = this->scale > 0 ? scale / this->scale : 1;
		double ox = server_.cursor->x - o->lx;
		double oy = server_.cursor->y - o->ly;
		o->overview_zoom_by(factor, ox, oy);
		this->scale = scale;
		break;
	}
	case GestureKind::pinch:
		this->scale = scale;
		if (scale > config::pinch_tile_scale) {
			merge_from = nullptr;
			if (desktop != nullptr) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "Release to tile Desktop %d",
					desktop->id);
				pinch_preview(buf);
			} else {
				pinch_preview("Release to tile windows here");
			}
		} else if (scale < config::pinch_combine_scale) {
			if (desktop == nullptr) {
				merge_from = nullptr;
				pinch_preview("Pinch over a desktop to combine");
			} else {
				merge_from = server_.desktop_nearest(*desktop);
				if (merge_from != nullptr) {
					char buf[80];
					std::snprintf(buf, sizeof(buf),
						"Release to combine Desktop %d into Desktop %d",
						merge_from->id, desktop->id);
					pinch_preview(buf);
				} else {
					pinch_clear_preview(false);
				}
			}
		} else {
			// Back in the neutral range: drop the preview.
			pinch_clear_preview(true);
		}
		o->damage();
		break;
	default:
		// Pending / decided swipe-like gestures: treat the travel of the
		// pinch as swipe motion. swipe_update updates last_time.
		swipe_update(time, dx, dy);
		return;
	}
	last_time = time;
}

void Gesture::pinch_end(uint32_t time, bool cancelled) {
	if (kind == GestureKind::forward) {
		wlr_pointer_gestures_v1_send_pinch_end(server_.pointer_gestures,
			server_.seat, time, cancelled);
		reset();
		return;
	}
	if (kind == GestureKind::pinch) {
		if (Output *o = output) {
			if (!cancelled && scale > config::pinch_tile_scale) {
				o->overview_tile(desktop);
			} else if (!cancelled && merge_from != nullptr) {
				o->overview_combine(desktop);
			}
			o->damage();
		}
		pinch_clear_preview(true);
		reset();
		return;
	}
	// Everything else (pending, pan, zoom, hop, abandon) releases like a
	// swipe; swipe_end resets the gesture state.
	swipe_end(time, cancelled);
}

void Gesture::hold_begin(uint32_t time, uint32_t fingers) {
	hold_forwarded_ = false;
	Output *out = server_.output_under_cursor();
	if (out == nullptr) {
		return;
	}
	// Touching the pad halts any coasting momentum.
	out->coasting = false;
	out->vel_x = 0;
	out->vel_y = 0;
	if (out->interactive()) {
		wlr_pointer_gestures_v1_send_hold_begin(server_.pointer_gestures,
			server_.seat, time, fingers);
		hold_forwarded_ = true;
	}
}

void Gesture::hold_end(uint32_t time, bool cancelled) {
	if (hold_forwarded_) {
		wlr_pointer_gestures_v1_send_hold_end(server_.pointer_gestures,
			server_.seat, time, cancelled);
		hold_forwarded_ = false;
	}
}
