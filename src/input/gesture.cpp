#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "canvas/desktop.h"
#include "platform/output.h"
#include "core/server.h"

namespace {

// Travel (touchpad units) before a pending gesture is classified.
constexpr double decide_threshold = 12.0;

template <typename... Ts>
struct overloaded : Ts... {
	using Ts::operator()...;
};
template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Super held right now (the same query seat_modifiers makes for buttons).
bool super_held(Server &server) {
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	return keyboard != nullptr
		&& (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO) != 0;
}

// The finger counts a preset binds: `hop` hops in either mode, `overview`
// opens the overview and steers it. The `off` preset binds none.
struct FingerCounts {
	uint32_t hop = 0, overview = 0;
};

FingerCounts fingers_of(const GestureOptions &options) {
	switch (options.swipes) {
	case SwipePreset::three_finger_hops:
		return {.hop = 3, .overview = 4};
	case SwipePreset::four_finger_hops:
		return {.hop = 4, .overview = 3};
	case SwipePreset::off:
		return {};
	}
	return {};
}

// The output a gesture state acts on, or null for the states without one.
Output *state_output(const Gestures::State &state) {
	return std::visit(overloaded{
		[](const Gestures::Pending &s) -> Output * { return s.output; },
		[](const Gestures::ZoomOut &s) -> Output * { return s.output; },
		[](const Gestures::ZoomIn &s) -> Output * { return s.output; },
		[](const Gestures::ZoomFurther &s) -> Output * { return s.output; },
		[](const Gestures::Hop &s) -> Output * { return s.output; },
		[](const Gestures::Abandon &s) -> Output * { return s.output; },
		[](const Gestures::DesktopScale &s) -> Output * { return s.output; },
		[](const Gestures::Pinch &s) -> Output * { return s.output; },
		[](const Gestures::OverviewZoom &s) -> Output * { return s.output; },
		[](const auto &) -> Output * { return nullptr; },
	}, state);
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

std::optional<DesktopId> Gestures::desktop_under(Output &out) {
	if (std::optional<CanvasPos> pos = server_.input.cursor_canvas_pos()) {
		if (Desktop *desktop = server_.canvas.desktops.at(pos->cx, pos->cy)) {
			return desktop->id();
		}
	}
	if (Desktop *target = server_.canvas.desktops.find(out.overview.target().desktop)) {
		return target->id();
	}
	return std::nullopt;
}

const Gestures::State &Gestures::state() const {
	return state_;
}

bool Gestures::zooming(const Output &output) const {
	return std::visit(overloaded{
		[&](const ZoomOut &s) { return s.output == &output; },
		[&](const ZoomIn &s) { return s.output == &output; },
		[&](const ZoomFurther &s) { return s.output == &output; },
		[&](const OverviewZoom &s) { return s.output == &output; },
		[&](const DesktopScale &s) { return s.output == &output; },
		[](const auto &) { return false; },
	}, state_);
}

bool Gestures::opening_overview(const Output &output) const {
	const ZoomOut *z = std::get_if<ZoomOut>(&state_);
	return z != nullptr && z->output == &output;
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
	FingerCounts counts = fingers_of(pending.options);
	double hop = pending.options.hops_away ? -1 : 1;
	double oy = pending.options.overview_down ? -ay : ay;

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
		state_ = ZoomOut{o, o->overview.home(), far, oy, 0, 0};
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
		state_ = ZoomIn{o, near, o->camera.current(), oy, 0, 0};
		return;
	}
	if (vertical && oy < 0) {
		state_ = ZoomFurther{o, o->camera.current().zoom, oy, 0};
		return;
	}
	std::optional<DesktopId> d = desktop_under(*o);
	if (!d.has_value()) {
		state_ = Ignored{};
		return;
	}
	state_ = Abandon{o, *d, 0};
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
	// The options a gesture begins with are the ones it keeps.
	FingerCounts counts = fingers_of(options_);
	if (fingers == counts.hop || fingers == counts.overview) {
		state_ = Pending{out, fingers, false, 0, 0, options_};
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
	double dt = static_cast<double>(time) - static_cast<double>(last_time_);

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
		[&](ZoomOut &z) {
			if (!just_decided) {
				z.ay += zy;
			}
			if (dt > 0) {
				z.vy = 0.8 * z.vy + 0.2 * (zy / dt);
			}
			z.progress = clampd(-z.ay / config::overview_swipe_distance, 0, 1);
			z.output->camera.jump(camera_zoom_path(z.near, z.far, z.progress));
		},
		[&](ZoomIn &z) {
			if (!just_decided) {
				z.ay += zy;
			}
			if (dt > 0) {
				z.vy = 0.8 * z.vy + 0.2 * (zy / dt);
			}
			z.progress = clampd(z.ay / config::overview_swipe_distance, 0, 1);
			z.output->camera.jump(camera_zoom_path(z.near, z.far, 1 - z.progress));
		},
		[&](ZoomFurther &z) {
			if (!just_decided) {
				z.ay += zy;
			}
			if (dt > 0) {
				z.vy = 0.8 * z.vy + 0.2 * (zy / dt);
			}
			Camera cam = z.output->camera.current();
			cam.zoom = clampd(z.start_zoom * std::exp(z.ay / 400),
				config::overview_min_zoom, 0.75);
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
		[&](ZoomOut &z) {
			Output &o = *z.output;
			if (!cancelled && (z.progress > config::overview_commit || z.vy < -0.5)) {
				o.overview.finish_enter(z.far, z.progress);
			} else {
				o.camera.animate_zoom(z.near, z.far, z.progress, 0);
			}
			o.damage();
		},
		[&](ZoomIn &z) {
			Output &o = *z.output;
			if (!cancelled && (z.progress > config::overview_commit || z.vy > 0.5)) {
				o.overview.finish_exit(z.near, z.far, 1 - z.progress);
			} else {
				o.camera.animate_zoom(z.near, z.far, 1 - z.progress, 1);
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
					o.overview.abandon(swiped->id());
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
	pinch_begin(time, fingers, super_held(server_));
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

	if (fingers == 2 && super && options_.pinch_scale) {
		// Super decides a 2-finger pinch at its start: it scales a
		// desktop and never reaches a client. A free view — and, in the
		// overview, a free-spot target — is pinned as a desktop first.
		bool overview = out->overview.active();
		Desktop *desktop = nullptr;
		if (overview) {
			// The desktop under the cursor, else the target.
			if (std::optional<CanvasPos> pos = server_.input.cursor_canvas_pos();
					pos.has_value() && pos->output == out) {
				desktop = server_.canvas.desktops.at(pos->cx, pos->cy);
			}
			if (desktop == nullptr) {
				if (Desktop *target =
						server_.canvas.desktops.find(out->overview.target().desktop);
						target != nullptr) {
					desktop = target;
				} else {
					out->overview.pin_target();
					desktop = server_.canvas.desktops.find(out->overview.target().desktop);
				}
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
		FingerCounts counts = fingers_of(options_);
		if (counts.hop == 0) {
			// No preset: the compositor owns every gesture and binds none.
			state_ = Ignored{};
			return;
		}
		if (fingers == counts.hop) {
			// A null target is allowed: tile then acts on the target box.
			state_ = Pinch{out, desktop_under(*out), std::nullopt, 1, false};
			return;
		}
		if (fingers == counts.overview
				|| fingers > std::max(counts.hop, counts.overview)) {
			// Travel drives the overview-finger swipe logic.
			state_ = Pending{out, fingers, true, 0, 0, options_};
			return;
		}
		state_ = Ignored{};
		return;
	}

	// Normal mode: a sloppy bound-finger swipe arrives as a pinch; treat
	// the travel as a swipe so it still hops / opens the overview. With no
	// preset the pinch is the client's, like a 2-finger one.
	FingerCounts counts = fingers_of(options_);
	if (fingers >= 3 && counts.hop != 0) {
		state_ = Pending{out, fingers, true, 0, 0, options_};
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

void Gestures::pinch_preview(Pinch &pinch, std::string_view text) {
	server_.hint.set(text);
	pinch.hinted = true;
}

void Gestures::pinch_clear_preview(Pinch &pinch, bool clear_merge) {
	if (clear_merge) {
		pinch.merge_from = std::nullopt;
	}
	if (pinch.hinted) {
		server_.hint.set({});
		pinch.hinted = false;
	}
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
		pinch->scale = scale;
		Desktop *pinched = server_.canvas.desktops.find(pinch->desktop);
		if (scale > config::pinch_tile_scale) {
			pinch->merge_from = std::nullopt;
			if (pinched != nullptr) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), pinched->tiling()
					? "Release to untile Desktop %d"
					: "Release to tile Desktop %d",
					pinched->number());
				pinch_preview(*pinch, buf);
			} else {
				pinch_preview(*pinch, "Release to tile windows here");
			}
		} else if (scale < config::pinch_combine_scale) {
			if (pinched == nullptr) {
				pinch->merge_from = std::nullopt;
				pinch_preview(*pinch, "Pinch over a desktop to combine");
			} else {
				Desktop *merge = server_.canvas.desktops.nearest(pinched->id());
				pinch->merge_from = merge != nullptr
					? std::optional<DesktopId>{merge->id()} : std::nullopt;
				if (merge != nullptr) {
					char buf[80];
					std::snprintf(buf, sizeof(buf),
						"Release to combine Desktop %d into Desktop %d",
						merge->number(), pinched->number());
					pinch_preview(*pinch, buf);
				} else {
					pinch_clear_preview(*pinch, false);
				}
			}
		} else {
			// Back in the neutral range: drop the preview.
			pinch_clear_preview(*pinch, true);
		}
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
				o->overview.combine(pinch->desktop);
			}
			o->damage();
		}
		pinch_clear_preview(*pinch, true);
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
