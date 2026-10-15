#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include "desktop.h"
#include "geometry.h"

class Output;
class Server;

enum class GestureKind {
	none,
	forward, // belongs to the client under the pointer
	pending, // compositor-owned, direction not decided yet
	pan, // normal mode: move the canvas
	zoom_out, // normal mode: 4 fingers up into the overview
	zoom_in, // overview: 4 fingers down into the target
	zoom_further, // overview: 4 fingers up, zoom out more
	hop, // overview: 3 fingers, jump to a desktop
	abandon, // overview: 4 fingers sideways
	pinch, // overview: 3-finger pinch, tile or combine
	overview_zoom, // overview: 2-finger pinch
	ignored,
};

// The touchpad gesture in progress.
class Gesture {
public:
	explicit Gesture(Server &server);
	Gesture(const Gesture &) = delete;
	Gesture &operator=(const Gesture &) = delete;

	void swipe_begin(uint32_t time, uint32_t fingers);
	void swipe_update(uint32_t time, double dx, double dy);
	void swipe_end(uint32_t time, bool cancelled);
	void pinch_begin(uint32_t time, uint32_t fingers);
	void pinch_update(uint32_t time, double dx, double dy, double scale, double rotation);
	void pinch_end(uint32_t time, bool cancelled);
	void hold_begin(uint32_t time, uint32_t fingers);
	void hold_end(uint32_t time, bool cancelled);

	// 0 unless that desktop is being swiped away.
	double abandon_offset(DesktopId desktop) const;

	GestureKind kind = GestureKind::none;
	bool pinch = false; // began as a pinch (matters for forwarding)
	uint32_t fingers = 0;
	Output *output = nullptr;
	double ax = 0, ay = 0; // accumulated travel
	double scale = 0; // pinch scale relative to begin
	double vx = 0, vy = 0; // velocity estimate, units per ms
	uint32_t last_time = 0;
	double progress = 0;
	double start_zoom = 0;
	Camera near; // zoom gestures: close-up end of the path
	Camera far; // zoom gestures: zoomed-out end
	std::optional<DesktopId> desktop; // acted on by abandon / pinch
	std::optional<DesktopId> merge_from; // pinch-in preview: absorbed desktop

private:
	void reset();
	// Classifies a pending gesture now that enough travel has accumulated.
	void decide();
	// The desktop under the cursor (canvas coords), or the overview target
	// desktop; nullopt once it is gone.
	std::optional<DesktopId> desktop_under(Output &output);
	void pinch_preview(std::string_view text);
	void pinch_clear_preview(bool clear_merge);

	double abandon_offset_ = 0; // abandon swipe: sideways travel of the swiped frame


	bool hop_done_ = false; // one hop per 3-finger gesture
	bool hold_forwarded_ = false; // whether the current hold gesture was forwarded to a client
	bool pinch_hinted_ = false; // whether this pinch currently shows a tile/combine preview hint

	Server &server_;
};
