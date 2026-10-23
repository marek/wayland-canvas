#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include "canvas/desktop.h"
#include "core/geometry.h"
#include "core/settings.h"

class Output;
class Server;

// What canvas.conf's [gestures] section chooses
// (docs/intents/input/gestures.md): the finger count that hops and the one
// that opens the overview, or no 3- and 4-finger gestures at all; whether
// Super + a 2-finger pinch scales a desktop; which way the swipe that opens
// the overview goes, and which way a swipe hops.
struct GestureOptions {
	settings::SwipePreset swipes = settings::SwipePreset::three_finger_hops;
	bool pinch_scale = true;
	bool overview_down = false; // the overview opens swiping down, goes in swiping up
	bool hops_away = false; // a swipe hops away from where the fingers point
};

// The touchpad gesture in progress, one state per kind of gesture.
class Gestures {
public:
	struct Idle {};
	struct Ignored {};
	struct Forwarded { bool pinch = false; }; // belongs to the client under the pointer
	struct Pending { Output *output; uint32_t fingers; bool pinch; double ax, ay; };
	// The swipe that enters the overview (up) or leaves it (down), following the
	// fingers along the camera path from `near` to `far`.
	struct Zoom { Output *output; bool entering; Camera near, far; double ay, progress, vy; };
	struct ZoomFurther { Output *output; double start_zoom, ay; };
	// A hop-finger swipe in either mode: one hop past the travel threshold.
	struct Hop { Output *output; double ax, ay; bool done; };
	struct Abandon { Output *output; DesktopId desktop; double offset; };
	// What releasing a pinch would do, as its hint says: the outcome and the
	// numbers of the desktops the text names.
	struct PinchOutcome {
		enum class Kind { none, tile, untile, tile_windows, combine_pick, combine };
		Kind kind = Kind::none;
		int number = 0; // the desktop the pinch is over
		int merge = 0; // the desktop that would join it
		bool operator==(const PinchOutcome &) const = default;
	};
	// `previewed` is the outcome the hint shows now.
	struct Pinch { Output *output; std::optional<DesktopId> desktop, merge_from; double scale;
		PinchOutcome previewed; };
	struct OverviewZoom { Output *output; double scale; };
	// Super + 2-finger pinch: scales a desktop, following the fingers.
	struct DesktopScale {
		Output *output;
		DesktopId desktop;  // the desktop being scaled
		Camera start;       // the camera when the pinch began (normal view)
		double start_scale; // the desktop's scale then
		double scale;       // the pinch scale since begin
		bool overview;      // the pinch began in the overview
	};
	using State = std::variant<Idle, Ignored, Forwarded, Pending, Zoom,
		ZoomFurther, Hop, Abandon, Pinch, OverviewZoom, DesktopScale>;

	explicit Gestures(Server &server);
	Gestures(const Gestures &) = delete;
	Gestures &operator=(const Gestures &) = delete;

	void swipe_begin(uint32_t time, uint32_t fingers);
	void swipe_update(uint32_t time, double dx, double dy);
	void swipe_end(uint32_t time, bool cancelled);
	void pinch_begin(uint32_t time, uint32_t fingers);
	// As above, with Super said rather than read from the keyboard: two
	// fingers on a touchscreen stand for it (docs/intents/input/touch-and-pen.md).
	void pinch_begin(uint32_t time, uint32_t fingers, bool super);
	void pinch_update(uint32_t time, double dx, double dy, double scale, double rotation);
	void pinch_end(uint32_t time, bool cancelled);
	void hold_begin(uint32_t time, uint32_t fingers);
	void hold_end(uint32_t time, bool cancelled);

	const State &state() const;
	bool zooming(const Output &output) const; // a zoom gesture on that output
	bool opening_overview(const Output &output) const; // the 4-finger zoom out, before it commits
	double abandon_offset(DesktopId desktop) const; // 0 unless that desktop is being swiped away
	const Pinch *pinch() const; // the pinch in progress, for the tile/combine preview
	void output_gone(const Output &output); // its output was unplugged

	// Takes effect from the next gesture; one in progress keeps going.
	void set_options(const GestureOptions &options);
	const GestureOptions &options() const;

private:
	// Classifies a pending gesture now that enough travel has accumulated;
	// replaces the state, so the pending struct must not be read afterwards.
	void decide(Pending &pending);
	// The desktop under the cursor when it is on `output`, else that
	// output's overview target desktop; null once there is neither.
	Desktop *desktop_under(Output &output);
	// Shows `outcome` as the hint when it differs from what the pinch shows;
	// otherwise only keeps the hint showing.
	void preview_pinch(Pinch &pinch, const PinchOutcome &outcome);

	Server &server_;
	State state_ = Idle{};
	uint32_t last_time_ = 0;
	bool hold_forwarded_ = false; // whether the current hold gesture was forwarded to a client
	GestureOptions options_;
	// The options the gesture in progress began with: it keeps them.
	GestureOptions begun_;
};
