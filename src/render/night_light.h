#pragma once

#include "core/util.h"

class Output;
class Server;

// The night light: the screen warmed by a colour temperature, and the wlr
// gamma-control protocol a client (wlsunset, gammastep) uses instead. The
// filter rides the output's own gamma ramp, so a screenshot stays in true
// colours; an output with no ramp (nested, headless) shows no night light
// (docs/intents/render/night-light.md).
class NightLight {
public:
	explicit NightLight(Server &server);
	~NightLight();
	NightLight(const NightLight &) = delete;
	NightLight &operator=(const NightLight &) = delete;

	void init();
	void finish();

	// canvas.conf's [session] values changed.
	void settings_changed();

	// The hardware path, called while an output's commit state is built:
	// sets the colour transform a client holds, else the built-in filter's,
	// and returns whether the output took it. False means the filter has to
	// be drawn into the frame instead.
	bool apply_to_output(Output &output, wlr_output_state &state);
	// Whether `output` can show a filter at all: it has a gamma ramp of
	// its own.
	static bool holds_a_ramp(const Output &output);

private:
	void handle_set_gamma(wlr_gamma_control_manager_v1_set_gamma_event *event);
	// The built-in filter for the settings in force, or nullptr when it is
	// off. Owned here; rebuilt when the temperature changes.
	wlr_color_transform *built_in();
	// The transform an output should be showing, client first.
	wlr_color_transform *wanted(Output &output);

	Server &server_;
	wlr_gamma_control_manager_v1 *manager_ = nullptr;
	wlr_color_transform *built_in_ = nullptr;
	int built_in_k_ = 0; // the temperature built_in_ was made for; 0: off
	Listener set_gamma_;
};

// The red, green and blue multipliers of a colour temperature in kelvin:
// 6500 K is daylight and leaves the screen alone, lower is warmer.
void night_light_multipliers(int kelvin, float out[3]);
