#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "desktop.h"
#include "geometry.h"
#include "layer.h"
#include "util.h"

class Server;

enum class AnimKind {
	none,
	lerp, // a -> b
	zoom, // camera_zoom_path(a = near, b = far, p0 -> p1)
};

struct Anim {
	AnimKind kind = AnimKind::none;
	Camera a, b;
	double p0 = 0, p1 = 0;
	double start = 0, duration = 0; // ms
};

enum class Mode {
	normal,
	overview,
};

// Camera that shows a desktop-sized box at zoom 1.
Camera camera_for_box(wlr_box box);

// A monitor: its own camera onto the canvas, its overview state and its layer surfaces.
class Output {
public:
	Output(Server &server, wlr_output *wlr);
	~Output();
	Output(const Output &) = delete;
	Output &operator=(const Output &) = delete;

	// output.cpp
	void damage();
	Rect viewport() const;
	Camera camera() const; // quantized: what is drawn and hit-tested
	Point local_to_canvas(double ox, double oy) const;
	Point canvas_to_local(double cx, double cy) const;
	wlr_box usable_canvas_box(wlr_box area) const;

	// canvas.cpp: camera motion
	void set_camera(const Camera &cam);
	void animate_camera(const Camera &to);
	void animate_zoom(const Camera &near, const Camera &far, double p0, double p1);
	void stop_camera();
	bool tick(double now); // advances animation and momentum; true while still moving
	bool is_panning() const;
	bool interactive() const; // false while the compositor owns the pointer (overview, animation)
	Desktop *desktop_showing() const;
	void goto_box(wlr_box box);
	void goto_direction(double dx, double dy);
	void pan(double dx, double dy);
	void pin_desktop();

	// canvas.cpp: overview
	Camera overview_camera(const Camera &home) const;
	void overview_enter();
	// Records home (the current zoom-1 camera) and the initial target; returns the overview
	// camera to zoom out to. Does not change the mode.
	Camera overview_prepare();
	// Gesture release: switch to overview and animate path parameter t -> 1.
	void overview_finish_enter(const Camera &far, double t);
	// Gesture release: switch to normal and animate path parameter t -> 0, landing on
	// `near` (camera_for_box(target)).
	void overview_finish_exit(const Camera &near, const Camera &far, double t);
	void overview_exit();
	void overview_cancel();
	void overview_hop(double dx, double dy);
	void overview_set_target(wlr_box box, std::optional<DesktopId> desktop, bool center);
	void overview_target_point(double cx, double cy);
	void overview_zoom_by(double factor, double ox, double oy);
	void overview_abandon(std::optional<DesktopId> desktop);
	void overview_tile(std::optional<DesktopId> desktop);
	void overview_combine(std::optional<DesktopId> into);
	void overview_new_desktop(double cx, double cy);

	// layer.cpp
	void arrange_layers();
	std::optional<LayerHit> layer_surface_at(int layer, double ox, double oy) const;
	bool has_wallpaper() const;

	Server &server;
	wlr_output *const wlr;
	uint32_t bit = 0;
	int lx = 0, ly = 0, width = 0, height = 0; // layout position and logical size

	Camera cam;
	Anim anim;
	double vel_x = 0, vel_y = 0; // momentum, canvas units per ms
	bool coasting = false;
	double last_tick = 0;
	double frames_alpha = 0; // desktop outlines faded in while panning

	Mode mode = Mode::normal;
	Camera home; // camera before entering the overview
	double overview_zoom = 0;
	// Overview target: where entering will take you. A desktop, or a free screen-sized
	// spot on the canvas when the target desktop resolves to nothing.
	wlr_box target{};
	std::optional<DesktopId> target_desktop;

	// By zwlr_layer_shell_v1_layer; each in the order the C wl_list kept.
	std::array<std::vector<std::unique_ptr<LayerSurface>>, 4> layers;
	wlr_box usable{}; // output-local area not covered by exclusive zones

	bool dirty = false;

private:
	void handle_frame();
	void handle_needs_frame();
	void handle_request_state(wlr_output_event_request_state *event);
	void handle_destroy();

	Listener frame_, needs_frame_, request_state_, destroy_;
};
