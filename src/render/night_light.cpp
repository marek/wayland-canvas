#include "render/night_light.h"

#include <algorithm>
#include <cmath>
#include "core/config.h"
#include "core/server.h"
#include "core/settings.h"
#include "platform/output.h"

namespace {

// A black body's colour at `kelvin`, Tanner Helland's approximation,
// normalised so 6500 K leaves every channel alone.
float channel(double value) {
	return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

} // namespace

void night_light_multipliers(int kelvin, float out[3]) {
	double temp = std::clamp(kelvin, settings::night_light_min_k,
		settings::night_light_max_k) / 100.0;
	double red = temp <= 66 ? 255.0
		: 329.698727446 * std::pow(temp - 60, -0.1332047592);
	double green = temp <= 66
		? 99.4708025861 * std::log(temp) - 161.1195681661
		: 288.1221695283 * std::pow(temp - 60, -0.0755148492);
	double blue = temp >= 66 ? 255.0
		: (temp <= 19 ? 0.0
			: 138.5177312231 * std::log(temp - 10) - 305.0447927307);
	out[0] = channel(red / 255.0);
	out[1] = channel(green / 255.0);
	out[2] = channel(blue / 255.0);
}

NightLight::NightLight(Server &server) : server_(server) {}

NightLight::~NightLight() {
	finish();
}

void NightLight::init() {
	manager_ = wlr_gamma_control_manager_v1_create(server_.display);
	if (manager_ == nullptr) {
		wlr_log(WLR_ERROR, "failed to create the gamma control manager");
		return;
	}
	set_gamma_.connect<&NightLight::handle_set_gamma>(manager_->events.set_gamma, this);
}

void NightLight::finish() {
	set_gamma_.disconnect();
	if (built_in_ != nullptr) {
		wlr_color_transform_unref(built_in_);
		built_in_ = nullptr;
		built_in_k_ = 0;
	}
}

void NightLight::handle_set_gamma(wlr_gamma_control_manager_v1_set_gamma_event *event) {
	// A client's table wins on its output while it holds it; the built-in
	// filter steps aside there and comes back when the client lets go.
	if (event->output != nullptr && event->output->data != nullptr) {
		static_cast<Output *>(event->output->data)->damage();
	}
}

wlr_color_transform *NightLight::built_in() {
	const settings::Resolved &conf = server_.conf;
	int kelvin = conf.night_light ? conf.night_light_k : 0;
	if (kelvin == built_in_k_) {
		return built_in_;
	}
	if (built_in_ != nullptr) {
		wlr_color_transform_unref(built_in_);
		built_in_ = nullptr;
	}
	built_in_k_ = kelvin;
	if (kelvin == 0) {
		return nullptr;
	}
	float rgb[3];
	night_light_multipliers(kelvin, rgb);
	const float matrix[9] = {
		rgb[0], 0, 0,
		0, rgb[1], 0,
		0, 0, rgb[2],
	};
	built_in_ = wlr_color_transform_init_matrix(matrix);
	return built_in_;
}

wlr_color_transform *NightLight::wanted(Output &output) {
	if (manager_ != nullptr) {
		wlr_gamma_control_v1 *control =
			wlr_gamma_control_manager_v1_get_control(manager_, output.wlr);
		if (control != nullptr) {
			return wlr_gamma_control_v1_get_color_transform(control);
		}
	}
	return built_in();
}

bool NightLight::apply_to_output(Output &output, wlr_output_state &state) {
	wlr_color_transform *transform = wanted(output);
	if (transform == nullptr) {
		return true; // nothing to show: the frame needs no transform either
	}
	if (wlr_output_get_gamma_size(output.wlr) == 0) {
		return false; // no ramp to hold it: the frame carries the filter
	}
	wlr_output_state pending{};
	if (!wlr_output_state_copy(&pending, &state)) {
		return false;
	}
	wlr_output_state_set_color_transform(&pending, transform);
	bool held = wlr_output_test_state(output.wlr, &pending);
	if (held) {
		wlr_output_state_set_color_transform(&state, transform);
	}
	wlr_output_state_finish(&pending);
	return held;
}

bool NightLight::holds_a_ramp(const Output &output) {
	return wlr_output_get_gamma_size(output.wlr) > 0;
}

void NightLight::settings_changed() {
	built_in(); // rebuilt for the new temperature
	server_.damage_all();
}
