#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "canvas/camera.h"
#include "canvas/desktop.h"
#include "core/geometry.h"
#include "platform/layer.h"
#include "canvas/overview.h"
#include "core/util.h"

class Server;

// A monitor: its own camera onto the canvas, its overview and its layer surfaces.
class Output {
public:
	Output(Server &server, wlr_output *wlr);
	~Output();
	Output(const Output &) = delete;
	Output &operator=(const Output &) = delete;

	// output.cpp
	void damage();
	// What this output shows, or where it looks, changed: repaints it and
	// has every window's outputs worked out again before the next frame.
	void visibility_changed();
	Rect viewport() const;
	Point local_to_canvas(double ox, double oy) const;
	Point canvas_to_local(double cx, double cy) const;
	// `area` on the canvas, shown at `zoom`, less what this output's bars
	// (exclusive zones) cover of it.
	wlr_box usable_canvas_box(wlr_box area, double zoom) const;
	bool tick(double now); // advances camera motion and the frame fade; true while still moving
	bool is_panning() const;
	bool interactive() const; // false while the compositor owns the pointer (overview, animation)
	Desktop *desktop_showing() const;
	double resting_zoom() const; // the zoom the camera rests at: the filling desktop's scale, else 1
	void goto_desktop(const Desktop &desktop); // eases onto it at its scale
	// The scale the clients of a surface shown on several outputs render
	// for: `scale` with one more output folded in (0 before the first), the
	// sharpest winning, with no floor (docs/intents/render/hidpi.md).
	static double sharper_scale(double scale, const wlr_output &output);
	// Tells `surface`'s client to render for `scale`: through
	// fractional-scale, and rounded up as the preferred buffer scale.
	static void tell_scale(wlr_surface *surface, double scale);
	void goto_direction(double dx, double dy);
	// A pan in the normal view is a look away
	// (docs/intents/canvas/infinite-canvas.md): the camera jumps to `to`,
	// and the view it rested at before the look away began is kept.
	void look_to(const Camera &to);
	void pan(double dx, double dy); // a look away by (dx, dy) canvas px
	// Eases back to the view the look away left; false when none is under
	// way — nothing panned, or the camera moved otherwise since.
	bool pan_back();
	void forget_look_away();
	void pin_desktop();

	// layer.cpp
	void arrange_layers();
	std::optional<LayerHit> layer_surface_at(int layer, double ox, double oy) const;
	// A plugin's surface on the plane under an output-local point.
	std::optional<LayerHit> canvas_surface_at(double ox, double oy) const;
	bool has_wallpaper() const;

	Server &server;
	wlr_output *const wlr;
	uint32_t bit = 0;
	int lx = 0, ly = 0, width = 0, height = 0; // layout position and logical size

	CameraMotion camera;
	Overview overview;
	double frames_alpha = 0; // desktop outlines faded in while panning

	// By zwlr_layer_shell_v1_layer; each in the order the C wl_list kept.
	std::array<std::vector<std::unique_ptr<LayerSurface>>, 4> layers;
	wlr_box usable{}; // output-local area not covered by exclusive zones

	bool dirty = false;

private:
	void handle_frame();
	void handle_needs_frame();
	void handle_request_state(wlr_output_event_request_state *event);
	void handle_destroy();
	double fade_last_ = 0; // clock for the desktop-frame fade
	// The view the camera rested at before the look away, and where the
	// last pan left the camera: moved anywhere else since, the look away
	// is over.
	struct LookAway {
		Camera from, left;
	};
	std::optional<LookAway> look_away_;

	Listener frame_, needs_frame_, request_state_, destroy_;
};
