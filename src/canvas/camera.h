#pragma once

#include "core/geometry.h"
#include "core/util.h"

class Output;

// The camera that shows `box` filling the screen at `zoom` (1: a free area).
Camera camera_for_box(wlr_box box, double zoom = 1);

// How one output's camera moves: jumps, eased pans, zoom transitions, momentum.
class CameraMotion {
public:
	explicit CameraMotion(Output &output);
	CameraMotion(const CameraMotion &) = delete;
	CameraMotion &operator=(const CameraMotion &) = delete;

	const Camera &current() const; // the exact camera
	// Where the running animation ends, momentum aside; the current camera
	// when none runs.
	Camera destination() const;
	// Quantized to the output's pixel grid: the camera drawn and hit-tested.
	Camera drawn() const;
	// Stops motion; marks the output dirty, refreshes visibility and pointer focus.
	void jump(const Camera &to);
	void animate_to(const Camera &to); // eased pan over config::anim_ms
	void animate_zoom(const Camera &near, const Camera &far, double p0, double p1);
	void coast(double vx, double vy); // momentum, canvas units per ms
	void stop(); // stops animation and momentum
	void stop_coast(); // touching the pad: stops the momentum only
	bool tick(double now); // advances; true while still moving
	bool coasting() const;
	bool panning() const; // coasting or an eased pan running
	bool zooming() const; // a zoom transition running

private:
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

	Output &output_;
	Camera cam_;
	Anim anim_;
	double vel_x_ = 0, vel_y_ = 0; // momentum, canvas units per ms
	bool coasting_ = false;
	double last_tick_ = 0;
};
