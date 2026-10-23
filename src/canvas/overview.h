#pragma once

#include <optional>
#include <utility>

#include "canvas/desktop.h"
#include "core/geometry.h"
#include "core/util.h"

class Output;

// Where entering the overview's target takes you: a desktop, or a free
// screen-sized spot on the canvas.
struct OverviewTarget {
	wlr_box box{};
	std::optional<DesktopId> desktop;
};

// One output's zoomed-out view of the canvas.
class Overview {
public:
	explicit Overview(Output &output);
	Overview(const Overview &) = delete;
	Overview &operator=(const Overview &) = delete;

	bool active() const;
	const OverviewTarget &target() const;
	const Camera &home() const; // the camera before entering
	double zoom() const;
	// The camera that leaves into the target: centred on its box at the
	// target desktop's scale, else at the zoom that fills the screen with
	// the box (a free spot).
	Camera target_camera() const;
	// The zoomed-out camera that shows the home viewport and every desktop
	// and window around it.
	Camera camera_for(const Camera &home) const;
	// Records home (the current camera at its resting zoom) and the initial
	// target; returns the camera to zoom out to. Does not activate.
	Camera prepare();
	void enter();
	// Gesture release: activate and animate path parameter t -> 1.
	void finish_enter(const Camera &far, double t);
	void exit();
	// Gesture release: deactivate and animate path parameter t -> 0, landing
	// on `near` (`target_camera()`).
	void finish_exit(const Camera &near, const Camera &far, double t);
	void cancel();
	void hop(double dx, double dy);
	void set_target(wlr_box box, std::optional<DesktopId> desktop, bool center);
	void target_point(double cx, double cy);
	void zoom_by(double factor, double ox, double oy);
	void keep_zoom(); // the camera's current zoom becomes the overview zoom
	void tile(std::optional<DesktopId> desktop);
	void new_desktop(double cx, double cy);
	void pin_target(); // the target spot becomes a desktop
	// Whether the overview was entered or left since the last call: the Ipc
	// tells its subscribers once per frame.
	bool take_changed() { return std::exchange(changed_, false); }

private:
	// The target the overview opens on and goes back to: the desktop the
	// output shows, else the screen-sized spot `home_` looks at.
	void reset_target();

	Output &output_;
	bool active_ = false;
	bool changed_ = false;
	Camera home_;
	double zoom_ = 0;
	OverviewTarget target_;
};
