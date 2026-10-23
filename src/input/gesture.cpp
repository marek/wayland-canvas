#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "canvas/desktop.h"
#include "core/server.h"
#include "core/util.h"
#include "platform/output.h"

namespace {

// Travel (touchpad units) before a pending gesture is classified.
constexpr double decide_threshold = 12.0;

// The finger counts a preset binds: `hop` hops in either mode, `overview`
// opens the overview and steers it. The `off` preset binds none.
struct FingerCounts {
	uint32_t hop = 0, overview = 0;
};

FingerCounts fingers_of(const GestureOptions &options) {
	switch (options.swipes) {
	case settings::SwipePreset::three_finger_hops:
		return {.hop = 3, .overview = 4};
	case settings::SwipePreset::four_finger_hops:
		return {.hop = 4, .overview = 3};
	case settings::SwipePreset::off:
		return {};
	}
	return {};
}

// The output a gesture state acts on. A state without one is named here, so
// a new state that has no `output` member fails to compile.
Output *state_output(const Gestures::State &state) {
	return std::visit(overloaded{
		[](const Gestures::Idle &) -> Output * { return nullptr; },
		[](const Gestures::Ignored &) -> Output * { return nullptr; },
		[](const Gestures::Forwarded &) -> Output * { return nullptr; },
		[](const auto &s) -> Output * { return s.output; },
	}, state);
}

// Up enters the overview, down leaves it.
double zoom_way(const Gestures::Zoom &z) {
	return z.entering ? -1 : 1;
}

// Where a zoom is on its camera path (0 at near, 1 at far) once the fingers
// have made `progress`: entering runs near to far, leaving far to near.
double zoom_path_t(const Gestures::Zoom &z, double progress) {
	return z.entering ? progress : 1 - progress;
}

double smoothed_velocity(double velocity, double sample) {
	return (1 - config::swipe_velocity_weight) * velocity
		+ config::swipe_velocity_weight * sample;
}

} // namespace

Gestures::Gestures(Server &server)
	: server_(server) {
}

void Gestures::set_options(const GestureOptions &options) {
	options_ = options;
}

const GestureOptions &Gestures::options() const {
	return options_;
}

Desktop *Gestures::desktop_under(Output &out) {
	if (Desktop *desktop = server_.input.desktop_under_cursor(out)) {
		return desktop;
	}
	return server_.canvas.desktops.find(out.overview.target().desktop);
}

const Gestures::State &Gestures::state() const {
	return state_;
}

bool Gestures::zooming(const Output &output) const {
	return std::visit(overloaded{
		[&](const Zoom &s) { return s.output == &output; },
		[&](const ZoomFurther &s) { return s.output == &output; },
		[&](const OverviewZoom &s) { return s.output == &output; },
		[&](const DesktopScale &s) { return s.output == &output; },
		[](const auto &) { return false; },
	}, state_);
}

bool Gestures::opening_overview(const Output &output) const {
	const Zoom *z = std::get_if<Zoom>(&state_);
	return z != nullptr && z->entering && z->output == &output;
}

double Gestures::abandon_offset(DesktopId id) const {
	if (const Abandon *abandon = std::get_if<Abandon>(&state_);
			abandon != nullptr && abandon->desktop == id) {
		return abandon->offset;
	}
	return 0;
}

const Gestures::Pinch *Gestures::pinch() const {
	return std::get_if<Pinch>(&state_);
}

void Gestures::output_gone(const Output &output) {
	// A forwarded gesture still owes its client the end event; idle and
	// ignored gestures hold nothing.
	if (state_output(state_) == &output) {
		state_ = Ignored{};
	}
}

void Gestures::decide(Pending &pending) {
	// The assignment below replaces the state, so read the pending fields
	// into locals first.
	Output *o = pending.output;
	uint32_t fingers = pending.fingers;
	double ax = pending.ax, ay = pending.ay;
	bool vertical = std::fabs(ay) > std::fabs(ax);

	// The preset the gesture began with decides what the fingers mean, and
	// its switches which way: the hop way for the hop fingers, the overview
	// way for the overview fingers' up and down.
	FingerCounts counts = fingers_of(begun_);
	double hop = begun_.hops_away ? -1 : 1;
	double oy = begun_.overview_down ? -ay : ay;

	// The hop fingers hop in either mode.
	if (fingers == counts.hop) {
		state_ = Hop{o, ax * hop, ay * hop, false};
		return;
	}
	// The rest is the overview fingers: in the normal view only a swipe
	// up, the zoom out into the overview.
	bool overview = o->overview.active();
	if (!overview && fingers == counts.overview && vertical && oy < 0) {
		Camera far = o->overview.prepare();
		state_ = Zoom{o, true, o->overview.home(), far, oy, 0, 0};
		return;
	}
	if (!overview || fingers != counts.overview) {
		state_ = Ignored{};
		return;
	}

	// Overview, the overview fingers: down zooms into the target, up zooms
	// further out, sideways abandons a desktop.
	if (vertical && oy > 0) {
		Camera near = o->overview.target_camera();
		state_ = Zoom{o, false, near, o->camera.current(), oy, 0, 0};
		return;
	}
	if (vertical && oy < 0) {
		state_ = ZoomFurther{o, o->camera.current().zoom, oy};
		return;
	}
	Desktop *desktop = desktop_under(*o);
	if (desktop == nullptr) {
		state_ = Ignored{};
		return;
	}
	state_ = Abandon{o, desktop->id(), 0};
}

void Gestures::swipe_begin(uint32_t time, uint32_t fingers) {
	Output *out = server_.output_under_cursor();
	state_ = Idle{};
	begun_ = options_;
	if (out == nullptr) {
		state_ = Ignored{};
		return;
	}
	last_time_ = time;
	FingerCounts counts = fingers_of(begun_);
	if (fingers == counts.hop || fingers == counts.overview) {
		state_ = Pending{out, fingers, false, 0, 0};
		return;
	}
	if (out->interactive()) {
		state_ = Forwarded{};
		wlr_pointer_gestures_v1_send_swipe_begin(server_.pointer_gestures,
			server_.seat, time, fingers);
		return;
	}
	state_ = Ignored{};
}

void Gestures::swipe_update(uint32_t time, double dx, double dy) {
	if (std::get_if<Forwarded>(&state_)) {
		wlr_pointer_gestures_v1_send_swipe_update(server_.pointer_gestures,
			server_.seat, time, dx, dy);
		return;
	}
	if (state_output(state_) == nullptr) {
		return;
	}
	// The hop fingers go the hop way, the overview fingers' up and down the
	// overview way, each by the options the gesture began with; a forwarded
	// gesture above keeps what libinput sent, and an abandon slide follows
	// the fingers.
	double hop_way = begun_.hops_away ? -1 : 1;
	double zy = begun_.overview_down ? -dy : dy;

	// A pending gesture accumulates travel until there is enough to
	// classify it; the decided state carries the travel it needs.
	bool just_decided = false;
	if (auto *pending = std::get_if<Pending>(&state_)) {
		pending->ax += dx;
		pending->ay += dy;
		if (std::hypot(pending->ax, pending->ay) <= decide_threshold) {
			last_time_ = time;
			return;
		}
		decide(*pending);
		just_decided = true;
	}

	std::visit(overloaded{
		[&](Hop &hop) {
			if (!just_decided) {
				hop.ax += dx * hop_way;
				hop.ay += dy * hop_way;
			}
			if (hop.done || std::hypot(hop.ax, hop.ay) <= config::hop_threshold) {
				return;
			}
			hop.done = true;
			// The same hop in both modes, the way the fingers point: the
			// overview moves its target, the normal view its camera.
			Output &o = *hop.output;
			if (o.overview.active()) {
				o.overview.hop(hop.ax, hop.ay);
				return;
			}
			o.goto_direction(hop.ax, hop.ay);
		},
		[&](Zoom &z) {
			if (!just_decided) {
				z.ay += zy;
			}
			double dt = static_cast<double>(time) - static_cast<double>(last_time_);
			if (dt > 0) {
				z.vy = smoothed_velocity(z.vy, zy / dt);
			}
			z.progress = clampd(zoom_way(z) * z.ay / config::overview_swipe_distance, 0, 1);
			z.output->camera.jump(
				camera_zoom_path(z.near, z.far, zoom_path_t(z, z.progress)));
		},
		[&](ZoomFurther &z) {
			if (!just_decided) {
				z.ay += zy;
			}
			Camera cam = z.output->camera.current();
			cam.zoom = clampd(z.start_zoom * std::exp(z.ay / config::overview_swipe_zoom_travel),
				config::overview_min_zoom, config::overview_zoom_limit);
			z.output->camera.jump(cam);
		},
		[&](Abandon &a) {
			if (Desktop *swiped = server_.canvas.desktops.find(a.desktop);
					swiped != nullptr) {
				if (!just_decided) {
					a.offset += dx;
				}
				a.output->damage();
			}
		},
		[](auto &) {},
	}, state_);
	last_time_ = time;
}

void Gestures::swipe_end(uint32_t time, bool cancelled) {
	if (auto *forwarded = std::get_if<Forwarded>(&state_)) {
		if (!forwarded->pinch) {
			wlr_pointer_gestures_v1_send_swipe_end(server_.pointer_gestures,
				server_.seat, time, cancelled);
		}
		state_ = Idle{};
		return;
	}

	std::visit(overloaded{
		[&](Zoom &z) {
			Output &o = *z.output;
			bool commit = !cancelled && (z.progress > config::overview_commit
				|| zoom_way(z) * z.vy > config::swipe_flick_velocity);
			double t = zoom_path_t(z, z.progress);
			if (!commit) {
				o.camera.animate_zoom(z.near, z.far, t, zoom_path_t(z, 0));
			} else if (z.entering) {
				o.overview.finish_enter(z.far, t);
			} else {
				o.overview.finish_exit(z.near, z.far, t);
			}
			o.damage();
		},
		[&](ZoomFurther &z) {
			z.output->overview.keep_zoom();
			z.output->damage();
		},
		[&](Abandon &a) {
			Output &o = *a.output;
			if (Desktop *swiped = server_.canvas.desktops.find(a.desktop);
					swiped != nullptr) {
				if (std::fabs(a.offset) > config::abandon_distance && !cancelled) {
					server_.canvas.abandon(swiped->id());
				} else {
					a.offset = 0;
					o.damage();
				}
			}
			o.damage();
		},
		// Hop, pending and overview-zoom gestures have no release action of
		// their own; the output is still damaged.
		[&](Hop &hop) { hop.output->damage(); },
		[&](Pending &pending) { pending.output->damage(); },
		[&](OverviewZoom &z) { z.output->damage(); },
		[](auto &) {},
	}, state_);
	state_ = Idle{};
}

void Gestures::pinch_begin(uint32_t time, uint32_t fingers) {
	pinch_begin(time, fingers, server_.input.super_held());
}

void Gestures::pinch_begin(uint32_t time, uint32_t fingers, bool super) {
	Output *out = server_.output_under_cursor();
	state_ = Idle{};
	begun_ = options_;
	if (out == nullptr) {
		state_ = Ignored{};
		return;
	}
	last_time_ = time;

	if (fingers == 2 && super && begun_.pinch_scale) {
		// Super decides a 2-finger pinch at its start: it scales a
		// desktop and never reaches a client. A free view — and, in the
		// overview, a free-spot target — is pinned as a desktop first.
		bool overview = out->overview.active();
		Desktop *desktop = nullptr;
		if (overview) {
			desktop = desktop_under(*out);
			if (desktop == nullptr) {
				out->overview.pin_target();
				desktop = server_.canvas.desktops.find(out->overview.target().desktop);
			}
		} else {
			desktop = out->desktop_showing();
			if (desktop == nullptr) {
				out->pin_desktop();
				desktop = out->desktop_showing();
			}
		}
		if (desktop == nullptr) {
			state_ = Ignored{};
			return;
		}
		out->camera.stop();
		state_ = DesktopScale{out, desktop->id(), out->camera.current(),
			desktop->scale(), 1, overview};
		return;
	}

	if (out->overview.active()) {
		if (fingers == 2) {
			state_ = OverviewZoom{out, 1};
			return;
		}
		FingerCounts counts = fingers_of(begun_);
		if (counts.hop == 0) {
			// No preset: the compositor owns every gesture and binds none.
			state_ = Ignored{};
			return;
		}
		if (fingers == counts.hop) {
			// A null target is allowed: tile then acts on the target box.
			state_ = Pinch{out, id_of(desktop_under(*out)), std::nullopt, 1, {}};
			return;
		}
		if (fingers == counts.overview
				|| fingers > std::max(counts.hop, counts.overview)) {
			// Travel drives the overview-finger swipe logic.
			state_ = Pending{out, fingers, true, 0, 0};
			return;
		}
		state_ = Ignored{};
		return;
	}

	// Normal mode: a sloppy bound-finger swipe arrives as a pinch; treat
	// the travel as a swipe so it still hops / opens the overview. With no
	// preset the pinch is the client's, like a 2-finger one.
	FingerCounts counts = fingers_of(begun_);
	if (fingers >= 3 && counts.hop != 0) {
		state_ = Pending{out, fingers, true, 0, 0};
		return;
	}
	if (fingers >= 2 && out->interactive()) {
		state_ = Forwarded{true};
		wlr_pointer_gestures_v1_send_pinch_begin(server_.pointer_gestures,
			server_.seat, time, fingers);
		return;
	}
	state_ = Ignored{};
}

void Gestures::preview_pinch(Pinch &pinch, const PinchOutcome &outcome) {
	using Kind = PinchOutcome::Kind;
	if (outcome == pinch.previewed) {
		if (outcome.kind != Kind::none) {
			server_.hint.keep();
		}
		return;
	}
	pinch.previewed = outcome;
	char text[80];
	switch (outcome.kind) {
	case Kind::none:
		server_.hint.set({});
		return;
	case Kind::tile:
		std::snprintf(text, sizeof(text), "Release to tile Desktop %d", outcome.number);
		break;
	case Kind::untile:
		std::snprintf(text, sizeof(text), "Release to untile Desktop %d", outcome.number);
		break;
	case Kind::tile_windows:
		std::snprintf(text, sizeof(text), "Release to tile windows here");
		break;
	case Kind::combine_pick:
		std::snprintf(text, sizeof(text), "Pinch over a desktop to combine");
		break;
	case Kind::combine:
		std::snprintf(text, sizeof(text), "Release to combine Desktop %d into Desktop %d",
			outcome.merge, outcome.number);
		break;
	}
	server_.hint.set(text);
}

void Gestures::pinch_update(uint32_t time, double dx, double dy,
		double scale, double rotation) {
	if (std::get_if<Forwarded>(&state_)) {
		wlr_pointer_gestures_v1_send_pinch_update(server_.pointer_gestures,
			server_.seat, time, dx, dy, scale, rotation);
		return;
	}
	if (state_output(state_) == nullptr) {
		return;
	}

	if (auto *z = std::get_if<OverviewZoom>(&state_)) {
		double factor = z->scale > 0 ? scale / z->scale : 1;
		double ox = server_.cursor->x - z->output->lx;
		double oy = server_.cursor->y - z->output->ly;
		z->output->overview.zoom_by(factor, ox, oy);
		z->scale = scale;
		last_time_ = time;
		return;
	}
	if (auto *z = std::get_if<DesktopScale>(&state_)) {
		// The scale follows the fingers: the frame grows or shrinks around
		// its centre, and in the normal view the camera zoom follows it.
		z->scale = scale > 0 ? scale : 1;
		double live = clampd(z->start_scale * z->scale,
			config::desktop_scale_min, config::desktop_scale_max);
		if (server_.canvas.desktops.find(z->desktop) == nullptr) {
			// The desktop was abandoned mid-pinch: the gesture is over.
			state_ = Idle{};
			return;
		}
		server_.canvas.desktops.set_scale(z->desktop, live);
		if (!z->overview) {
			Camera next = z->start;
			next.zoom = live;
			z->output->camera.jump(next);
		}
		last_time_ = time;
		return;
	}
	if (auto *pinch = std::get_if<Pinch>(&state_)) {
		using Kind = PinchOutcome::Kind;
		pinch->scale = scale;
		pinch->merge_from = std::nullopt;
		Desktop *pinched = server_.canvas.desktops.find(pinch->desktop);
		PinchOutcome outcome;
		if (scale > config::pinch_tile_scale) {
			if (pinched == nullptr) {
				outcome.kind = Kind::tile_windows;
			} else {
				outcome = {.kind = pinched->tiling() ? Kind::untile : Kind::tile,
					.number = pinched->number()};
			}
		} else if (scale < config::pinch_combine_scale) {
			if (pinched == nullptr) {
				outcome.kind = Kind::combine_pick;
			} else if (Desktop *merge = server_.canvas.desktops.nearest(pinched->id())) {
				pinch->merge_from = merge->id();
				outcome = {.kind = Kind::combine, .number = pinched->number(),
					.merge = merge->number()};
			}
		}
		preview_pinch(*pinch, outcome);
		pinch->output->damage();
		last_time_ = time;
		return;
	}
	// Pending / decided swipe-like gestures: treat the travel of the
	// pinch as swipe motion. swipe_update updates last_time_.
	swipe_update(time, dx, dy);
}

void Gestures::pinch_end(uint32_t time, bool cancelled) {
	if (auto *forwarded = std::get_if<Forwarded>(&state_)) {
		if (forwarded->pinch) {
			wlr_pointer_gestures_v1_send_pinch_end(server_.pointer_gestures,
				server_.seat, time, cancelled);
		}
		state_ = Idle{};
		return;
	}
	if (auto *pinch = std::get_if<Pinch>(&state_)) {
		if (Output *o = pinch->output) {
			if (!cancelled && pinch->scale > config::pinch_tile_scale) {
				o->overview.tile(pinch->desktop);
			} else if (!cancelled
					&& server_.canvas.desktops.find(pinch->merge_from) != nullptr) {
				server_.canvas.combine_nearest(pinch->desktop);
			}
			o->damage();
		}
		preview_pinch(*pinch, {});
		state_ = Idle{};
		return;
	}
	if (auto *z = std::get_if<DesktopScale>(&state_)) {
		Output &o = *z->output;
		Desktop *desktop = server_.canvas.desktops.find(z->desktop);
		if (desktop != nullptr) {
			// Released, the scale settles on the nearest step; 100 % is
			// one of the steps, so a near-100 pinch catches it exactly. A
			// cancelled pinch springs back to where it began.
			double settled = z->start_scale;
			if (!cancelled) {
				settled = clampd(
					std::round(z->start_scale * z->scale / config::desktop_scale_step)
						* config::desktop_scale_step,
					config::desktop_scale_min, config::desktop_scale_max);
			}
			server_.canvas.settle_scale(z->desktop, settled);
			if (!z->overview) {
				// The camera comes to rest on the desktop, at its scale.
				o.camera.animate_to(camera_for_box(desktop->frame(), settled));
			}
		}
		o.damage();
		state_ = Idle{};
		return;
	}
	// Everything else (pending, pan, zoom, hop, abandon) releases like a
	// swipe; swipe_end resets the gesture state.
	swipe_end(time, cancelled);
}

void Gestures::hold_begin(uint32_t time, uint32_t fingers) {
	hold_forwarded_ = false;
	Output *out = server_.output_under_cursor();
	if (out == nullptr) {
		return;
	}
	if (out->interactive()) {
		wlr_pointer_gestures_v1_send_hold_begin(server_.pointer_gestures,
			server_.seat, time, fingers);
		hold_forwarded_ = true;
	}
}

void Gestures::hold_end(uint32_t time, bool cancelled) {
	if (hold_forwarded_) {
		wlr_pointer_gestures_v1_send_hold_end(server_.pointer_gestures,
			server_.seat, time, cancelled);
		hold_forwarded_ = false;
	}
}
