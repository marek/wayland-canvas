#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include "canvas/desktop.h"
#include "core/geometry.h"

class Output;
class Server;

// The touchpad gesture in progress, one state per kind of gesture.
class Gestures {
public:
	struct Idle {};
	struct Ignored {};
	struct Forwarded { bool pinch = false; }; // belongs to the client under the pointer
	struct Pending { Output *output; uint32_t fingers; bool pinch; double ax, ay; };
	struct Pan { Output *output; double vx, vy; };
	struct ZoomOut { Output *output; Camera near, far; double ay, progress, vy; };
	struct ZoomIn { Output *output; Camera near, far; double ay, progress, vy; };
	struct ZoomFurther { Output *output; double start_zoom, ay, vy; };
	struct Hop { Output *output; double ax, ay; bool done; };
	struct Abandon { Output *output; DesktopId desktop; double offset; };
	struct Pinch { Output *output; std::optional<DesktopId> desktop, merge_from; double scale; bool hinted; };
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
	using State = std::variant<Idle, Ignored, Forwarded, Pending, Pan, ZoomOut, ZoomIn,
		ZoomFurther, Hop, Abandon, Pinch, OverviewZoom, DesktopScale>;

	explicit Gestures(Server &server);
	Gestures(const Gestures &) = delete;
	Gestures &operator=(const Gestures &) = delete;

	void swipe_begin(uint32_t time, uint32_t fingers);
	void swipe_update(uint32_t time, double dx, double dy);
	void swipe_end(uint32_t time, bool cancelled);
	void pinch_begin(uint32_t time, uint32_t fingers);
	void pinch_update(uint32_t time, double dx, double dy, double scale, double rotation);
	void pinch_end(uint32_t time, bool cancelled);
	void hold_begin(uint32_t time, uint32_t fingers);
	void hold_end(uint32_t time, bool cancelled);

	const State &state() const;
	bool panning(const Output &output) const;
	bool zooming(const Output &output) const; // a zoom gesture on that output
	double abandon_offset(DesktopId desktop) const; // 0 unless that desktop is being swiped away
	const Pinch *pinch() const; // the pinch in progress, for the tile/combine preview
	void output_gone(const Output &output); // its output was unplugged

private:
	// Classifies a pending gesture now that enough travel has accumulated;
	// replaces the state, so the pending struct must not be read afterwards.
	void decide(Pending &pending);
	// The desktop under the cursor (canvas coords), or the overview target
	// desktop; nullopt once it is gone.
	std::optional<DesktopId> desktop_under(Output &output);
	void pinch_preview(Pinch &pinch, std::string_view text);
	void pinch_clear_preview(Pinch &pinch, bool clear_merge);

	Server &server_;
	State state_ = Idle{};
	uint32_t last_time_ = 0;
	bool hold_forwarded_ = false; // whether the current hold gesture was forwarded to a client
};
