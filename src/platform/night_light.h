#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "core/util.h"

class Output;
class Server;

// The night light: the screen warmed by a colour temperature, or by the table
// a gamma-control client (wlsunset, gammastep) sets instead. It is one colour
// transform per output with a gamma ramp, committed with the output's state so
// a screenshot stays in true colours (docs/intents/render/night-light.md).
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

	// Called while an output's commit state is built: puts the transform the
	// output should show into `state`. An output with no ramp is left alone.
	void apply_to_output(Output &output, wlr_output_state &state);
	// Whether `output` can show a filter at all: it has a gamma ramp of
	// its own.
	static bool holds_a_ramp(const Output &output);

private:
	struct Slot; // what one output shows (night_light.cpp)

	void handle_set_gamma(wlr_gamma_control_manager_v1_set_gamma_event *event);
	Slot *find_slot(wlr_output *output);
	Slot &slot_for(wlr_output *output);

	Server &server_;
	wlr_gamma_control_manager_v1 *manager_ = nullptr;
	std::vector<std::unique_ptr<Slot>> slots_;
	Listener set_gamma_;
};
