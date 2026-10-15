#include "camera.h"

#include <cmath>

#include "config.h"
#include "output.h"
#include "server.h"

namespace {

// Momentum slower than this (canvas px per ms) is indistinguishable from
// still and ends the coast.
constexpr double coast_stop_speed = 0.02;

} // namespace

CameraMotion::CameraMotion(Output &output)
	: output_(output) {
}

const Camera &CameraMotion::current() const {
	return cam_;
}

Camera CameraMotion::drawn() const {
	Camera cam = cam_;
	if (cam.zoom == 1) {
		// At 1:1 keep window pixels on the output's pixel grid: no blur.
		double scale = output_.wlr->scale;
		double left = std::round((cam.cx - output_.width / 2.0) * scale) / scale;
		double top = std::round((cam.cy - output_.height / 2.0) * scale) / scale;
		cam.cx = left + output_.width / 2.0;
		cam.cy = top + output_.height / 2.0;
	}
	return cam;
}

void CameraMotion::halt() {
	anim_.kind = AnimKind::none;
	coasting_ = false;
	vel_x_ = 0;
	vel_y_ = 0;
	// Restart the tick clock so the next tick's dt is a real interval.
	last_tick_ = now_ms();
}

void CameraMotion::jump(const Camera &to) {
	halt();
	cam_ = to;
	output_.server.visibility_dirty = true;
	output_.server.pointer_focus_stale = true;
	output_.damage();
}

void CameraMotion::animate_to(const Camera &to) {
	halt();
	anim_.kind = AnimKind::lerp;
	anim_.a = cam_;
	anim_.b = to;
	anim_.start = now_ms();
	anim_.duration = config::anim_ms;
	output_.damage();
}

void CameraMotion::animate_zoom(const Camera &near, const Camera &far,
		double p0, double p1) {
	halt();
	anim_.kind = AnimKind::zoom;
	anim_.a = near;
	anim_.b = far;
	anim_.p0 = p0;
	anim_.p1 = p1;
	anim_.start = now_ms();
	anim_.duration = config::anim_ms * clampd(std::fabs(p1 - p0), 0.4, 1);
	output_.damage();
}

void CameraMotion::coast(double vx, double vy) {
	vel_x_ = vx;
	vel_y_ = vy;
	coasting_ = true;
	last_tick_ = now_ms();
	output_.damage();
}

bool CameraMotion::tick(double now) {
	double dt = now - last_tick_;
	if (!(dt >= 0)) {
		dt = 0;
	}
	if (dt > 50) {
		dt = 50;
	}
	last_tick_ = now;

	Camera next = cam_;
	bool busy = false;

	if (anim_.kind != AnimKind::none) {
		double t = (now - anim_.start) / anim_.duration;
		if (t >= 1) {
			// Land exactly on the end camera, no float drift.
			if (anim_.kind == AnimKind::lerp || anim_.p1 >= 1) {
				next = anim_.b;
			} else if (anim_.p1 <= 0) {
				next = anim_.a;
			} else {
				next = camera_zoom_path(anim_.a, anim_.b, anim_.p1);
			}
			anim_.kind = AnimKind::none;
		} else {
			double e = ease_out_cubic(t);
			if (anim_.kind == AnimKind::lerp) {
				next = camera_lerp(anim_.a, anim_.b, e);
			} else {
				double p = anim_.p0 + (anim_.p1 - anim_.p0) * e;
				next = camera_zoom_path(anim_.a, anim_.b, p);
			}
			busy = true;
		}
	}

	if (coasting_) {
		next.cx += vel_x_ * dt;
		next.cy += vel_y_ * dt;
		double decay = std::exp(-dt / config::inertia_tau_ms);
		vel_x_ *= decay;
		vel_y_ *= decay;
		if (std::hypot(vel_x_, vel_y_) < coast_stop_speed) {
			coasting_ = false;
			vel_x_ = 0;
			vel_y_ = 0;
		} else {
			busy = true;
		}
	}

	bool cam_changed = next.cx != cam_.cx || next.cy != cam_.cy || next.zoom != cam_.zoom;
	cam_ = next;

	if (cam_changed) {
		output_.dirty = true;
		output_.server.visibility_dirty = true;
		output_.server.pointer_focus_stale = true;
	}
	if (cam_changed || busy) {
		output_.damage();
	}

	return busy;
}

bool CameraMotion::coasting() const {
	return coasting_;
}

bool CameraMotion::panning() const {
	return coasting_ || anim_.kind == AnimKind::lerp;
}

bool CameraMotion::zooming() const {
	return anim_.kind == AnimKind::zoom;
}

Camera camera_for_box(wlr_box box) {
	return Camera{
		.cx = box.x + box.width / 2.0,
		.cy = box.y + box.height / 2.0,
		.zoom = 1,
	};
}
