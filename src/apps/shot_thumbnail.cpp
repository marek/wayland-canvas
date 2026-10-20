#include "apps/shot_thumbnail.h"

#include <linux/input-event-codes.h>

#include <cmath>

#include "core/config.h"
#include "core/server.h"
#include "core/theme.h"
#include "platform/output.h"
#include "render/draw.h"

namespace {

// Single-quotes `text` for /bin/sh.
std::string shell_quote(const std::string &text) {
	std::string out = "'";
	for (const char ch : text) {
		out += ch == '\'' ? "'\\''" : std::string(1, ch);
	}
	return out + "'";
}

} // namespace

ShotThumbnail::ShotThumbnail(Server &server) : server_(server) {
}

bool ShotThumbnail::over(const Output &output, double ox, double oy) const {
	return output_ == &output && wlr_box_contains_point(&box_, ox, oy);
}

void ShotThumbnail::show(const std::string &path, Output &output, cairo_surface_t *shot) {
	texture_.reset();
	texture_ = texture_from_cairo(server_.renderer, shot);
	int dev_w = texture_ ? cairo_image_surface_get_width(shot) : 0;
	int dev_h = texture_ ? cairo_image_surface_get_height(shot) : 0;
	if (dev_w <= 0 || dev_h <= 0) {
		texture_.reset();
		return;
	}
	path_ = path;
	output_ = &output;
	double ar = static_cast<double>(dev_w) / dev_h;
	double w = std::fmin(config::capture_thumbnail_max_w,
		config::capture_thumbnail_max_h * ar);
	double h = w / ar;
	box_ = {
		static_cast<int>(std::round(output.width - config::capture_thumbnail_margin - w)),
		static_cast<int>(std::round(output.height - config::capture_thumbnail_margin - h)),
		static_cast<int>(std::round(w)),
		static_cast<int>(std::round(h)),
	};
	if (!timer_) {
		timer_.reset(wl_event_loop_add_timer(server_.loop,
			[](void *data) -> int {
				static_cast<ShotThumbnail *>(data)->dismiss();
				return 0;
			}, this));
	}
	if (timer_) {
		wl_event_source_timer_update(timer_.get(), config::capture_thumbnail_ms);
	}
	output.damage();
}

void ShotThumbnail::dismiss() {
	if (texture_ == nullptr && output_ == nullptr) {
		return;
	}
	texture_.reset();
	path_.clear();
	Output *output = output_;
	output_ = nullptr;
	timer_.reset();
	pressed_ = false;
	if (output != nullptr) {
		output->damage();
	}
}

bool ShotThumbnail::takes_pointer_at(const Output &output, double ox, double oy) const {
	return texture_ != nullptr && (pressed_ || over(output, ox, oy));
}

bool ShotThumbnail::pointer_motion(Output &output, double ox, double oy) {
	if (texture_ == nullptr) {
		return false;
	}
	if (pressed_) {
		// A drag past the threshold — a swipe — sends it away.
		if (&output == output_) {
			double dx = ox - press_x_, dy = oy - press_y_;
			if (std::hypot(dx, dy) > config::drag_threshold) {
				dismiss();
			}
		} else {
			dismiss(); // dragged onto another output
		}
		return true;
	}
	return over(output, ox, oy);
}

bool ShotThumbnail::pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed) {
	if (texture_ == nullptr) {
		return false;
	}
	bool inside = over(output, ox, oy);
	if (button != BTN_LEFT) {
		return inside;
	}
	if (pressed) {
		if (!inside) {
			return false;
		}
		pressed_ = true;
		press_x_ = ox;
		press_y_ = oy;
		return true;
	}
	if (!pressed_) {
		return false;
	}
	pressed_ = false;
	if (inside) {
		spawn((std::string{"xdg-open "} + shell_quote(path_)).c_str());
	}
	return true;
}

void ShotThumbnail::add_overlay(Output &output, CaptureOverlay &out) {
	if (texture_ == nullptr || output_ != &output) {
		return;
	}
	out.images.push_back({.texture = texture_.get(), .box = box_});
	out.outlines.push_back({.box = box_,
		.rgba = server_.theme.colors().launcher_border, .thickness = 1.0});
}

void ShotThumbnail::output_destroyed(Output &output) {
	if (output_ == &output) {
		dismiss();
	}
}
