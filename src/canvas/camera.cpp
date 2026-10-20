#include "canvas/camera.h"

#include <cmath>
#include <type_traits>

#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"

CameraMotion::CameraMotion(Output &output)
	: output_(output) {
}

const Camera &CameraMotion::current() const {
	return cam_;
}

Camera CameraMotion::drawn() const {
	Camera cam = cam_;
	// Snap the top-left corner onto the output's pixel grid at this zoom:
	// a window rendered for its desktop's scale then maps its buffer
	// pixels onto device pixels one to one, never subpixel-blurry. Hit
	// testing goes through the same snapped camera.
	double z = cam.zoom > 0 ? cam.zoom : 1.0;
	double k = z * output_.wlr->scale; // device pixels per canvas unit
	double left = cam.cx - output_.width / (2.0 * z);
	double top = cam.cy - output_.height / (2.0 * z);
	cam.cx = std::round(left * k) / k + output_.width / (2.0 * z);
	cam.cy = std::round(top * k) / k + output_.height / (2.0 * z);
	return cam;
}

void CameraMotion::stop() {
	motion_ = Still{};
}

void CameraMotion::jump(const Camera &to) {
	stop();
	cam_ = to;
	output_.visibility_changed();
	output_.server.pointer_focus_stale = true;
}

void CameraMotion::animate_to(const Camera &to) {
	motion_ = Pan{.from = cam_, .to = to, .start = now_ms(), .duration = config::anim_ms};
	output_.damage();
}

void CameraMotion::animate_zoom(const Camera &near, const Camera &far,
		double p0, double p1) {
	motion_ = Zoom{
		.near = near,
		.far = far,
		.p0 = p0,
		.p1 = p1,
		.start = now_ms(),
		.duration = config::anim_ms * clampd(std::fabs(p1 - p0), 0.4, 1),
	};
	output_.damage();
}

Camera CameraMotion::Pan::at(double eased) const {
	return camera_lerp(from, to, eased);
}

Camera CameraMotion::Pan::end() const {
	return to;
}

Camera CameraMotion::Zoom::at(double eased) const {
	return camera_zoom_path(near, far, p0 + (p1 - p0) * eased);
}

// Exactly the end camera, no float drift from the path.
Camera CameraMotion::Zoom::end() const {
	if (p1 >= 1) {
		return far;
	}
	if (p1 <= 0) {
		return near;
	}
	return camera_zoom_path(near, far, p1);
}

bool CameraMotion::tick(double now) {
	Camera next = cam_;
	bool busy = false;

	std::visit([&]<typename Running>(const Running &motion) {
		if constexpr (!std::is_same_v<Running, Still>) {
			double t = (now - motion.start) / motion.duration;
			busy = t < 1;
			next = busy ? motion.at(ease_out_cubic(t)) : motion.end();
		}
	}, motion_);
	if (!busy) {
		motion_ = Still{};
	}

	bool cam_changed = next.cx != cam_.cx || next.cy != cam_.cy || next.zoom != cam_.zoom;
	cam_ = next;

	if (cam_changed) {
		output_.visibility_changed();
		output_.server.pointer_focus_stale = true;
	} else if (busy) {
		output_.damage();
	}

	return busy;
}

bool CameraMotion::panning() const {
	return std::holds_alternative<Pan>(motion_);
}

bool CameraMotion::zooming() const {
	return std::holds_alternative<Zoom>(motion_);
}

Camera CameraMotion::destination() const {
	return std::visit([this]<typename Running>(const Running &motion) {
		if constexpr (std::is_same_v<Running, Still>) {
			return cam_;
		} else {
			return motion.end();
		}
	}, motion_);
}

Camera camera_for_box(wlr_box box, double zoom) {
	return Camera{
		.cx = box.x + box.width / 2.0,
		.cy = box.y + box.height / 2.0,
		.zoom = zoom,
	};
}
