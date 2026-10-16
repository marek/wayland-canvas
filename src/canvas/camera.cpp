#include "canvas/camera.h"

#include <cmath>

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
	anim_.kind = AnimKind::none;
}

void CameraMotion::jump(const Camera &to) {
	stop();
	cam_ = to;
	output_.server.visibility_dirty = true;
	output_.server.pointer_focus_stale = true;
	output_.damage();
}

void CameraMotion::animate_to(const Camera &to) {
	stop();
	anim_.kind = AnimKind::lerp;
	anim_.a = cam_;
	anim_.b = to;
	anim_.start = now_ms();
	anim_.duration = config::anim_ms;
	output_.damage();
}

void CameraMotion::animate_zoom(const Camera &near, const Camera &far,
		double p0, double p1) {
	stop();
	anim_.kind = AnimKind::zoom;
	anim_.a = near;
	anim_.b = far;
	anim_.p0 = p0;
	anim_.p1 = p1;
	anim_.start = now_ms();
	anim_.duration = config::anim_ms * clampd(std::fabs(p1 - p0), 0.4, 1);
	output_.damage();
}

bool CameraMotion::tick(double now) {
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

bool CameraMotion::panning() const {
	return anim_.kind == AnimKind::lerp;
}

bool CameraMotion::zooming() const {
	return anim_.kind == AnimKind::zoom;
}

Camera CameraMotion::destination() const {
	switch (anim_.kind) {
	case AnimKind::lerp:
		return anim_.b;
	case AnimKind::zoom:
		if (anim_.p1 >= 1) {
			return anim_.b;
		}
		if (anim_.p1 <= 0) {
			return anim_.a;
		}
		return camera_zoom_path(anim_.a, anim_.b, anim_.p1);
	case AnimKind::none:
		break;
	}
	return cam_;
}

Camera camera_for_box(wlr_box box, double zoom) {
	return Camera{
		.cx = box.x + box.width / 2.0,
		.cy = box.y + box.height / 2.0,
		.zoom = zoom,
	};
}
