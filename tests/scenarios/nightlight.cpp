// End-to-end night-light scenario: the settings that hold the filter
// (`settings.set`, the toggle action and canvas.conf), the temperature's
// own multipliers, and the wlr gamma-control protocol answered by a client
// of the scenarios' own (tests/clients/gamma-client.c). A headless output
// holds no gamma ramp, so first nothing here warms a frame; then the output
// is given a ramp and a record of the colour transforms committed with its
// frames, as the DRM backend would take them, and the filter's life on it is
// followed: the built-in filter, a client's table, the client letting go.
// Writes only under $CANVAS_TMP, with canvas.conf in its own scratch config
// dir.
#include <array>
#include <csignal>
#include <cstdlib>
#include <format>
#include <linux/input-event-codes.h>
#include <string>
#include <string_view>
#include <vector>
#include "core/config.h"
#include "core/server.h"
#include "core/process.h"
#include "core/settings.h"
#include "core/wlr.h"
#include <wlr/interfaces/wlr_output.h>
#include "harness/check.h"
#include "harness/child.h"
#include "harness/devices.h"
#include "harness/image.h"
#include "harness/run.h"
#include "platform/night_light.h"
#include "platform/night_light_ramp.h"
#include "platform/output.h"

using namespace harness;

namespace {

Child gamma; // the gamma-control client

// The plain frame, to measure the others against: the output the compositor
// clears itself would be dark, so frames are compared with each other
// rather than with an absolute.
std::array<double, 3> plain{};

// Sets one [session] value the way the settings window does.
void set_session(std::string_view params) {
	spawn(std::format("'{}' msg settings.set '{}' >/dev/null 2>&1",
		std::getenv("WAYLAND_CANVAS_BIN"), params).c_str());
}

// The headless output with a gamma ramp: its own test and commit, behind a
// ramp size and a record of the colour transforms its commits carry. The
// headless backend takes none, so the transform is dropped before it sees
// the state.
constexpr std::size_t ramp_size = 256;
const wlr_output_impl *headless_impl = nullptr;
wlr_output_impl ramp_impl{};
std::vector<const wlr_color_transform *> ramp_commits;
bool refuse_tables = false; // a test of a state that is only a transform fails

bool forward(wlr_output *output, const wlr_output_state *state, bool commit) {
	wlr_output_state bare;
	wlr_output_state_init(&bare);
	bool ok = wlr_output_state_copy(&bare, state);
	bare.committed &= ~WLR_OUTPUT_STATE_COLOR_TRANSFORM;
	if (ok) {
		// The headless functions assert it is their own output.
		output->impl = headless_impl;
		ok = commit ? headless_impl->commit(output, &bare)
			: headless_impl->test(output, &bare);
		output->impl = &ramp_impl;
	}
	wlr_output_state_finish(&bare);
	return ok;
}

bool ramp_test(wlr_output *output, const wlr_output_state *state) {
	if (refuse_tables && state->committed == WLR_OUTPUT_STATE_COLOR_TRANSFORM) {
		return false;
	}
	return forward(output, state, false);
}

bool ramp_commit(wlr_output *output, const wlr_output_state *state) {
	if (!forward(output, state, true)) {
		return false;
	}
	if (state->committed & WLR_OUTPUT_STATE_COLOR_TRANSFORM) {
		ramp_commits.push_back(state->color_transform);
	}
	return true;
}

size_t ramp_gamma_size(wlr_output *) {
	return ramp_size;
}

void ramp_destroy(wlr_output *output) {
	output->impl = headless_impl;
	headless_impl->destroy(output);
}

void give_a_ramp(wlr_output *output) {
	headless_impl = output->impl;
	ramp_impl = *headless_impl;
	ramp_impl.test = ramp_test;
	ramp_impl.commit = ramp_commit;
	ramp_impl.get_gamma_size = ramp_gamma_size;
	ramp_impl.destroy = ramp_destroy;
	output->impl = &ramp_impl;
}

// ------------------------------------------------ the built-in filter

Result window_on_the_output() {
	attach_keyboard();
	spawn("foot -T night sh -c 'printf \"\\033[47m\\033[2J\"; exec sleep 600'");
	return after(2000);
}

Result night_light_starts_off() {
	if (auto wait = wait_for(!server->views.empty())) {
		return *wait;
	}
	check(!server->views.empty(), "a window on the output");
	check(!server->conf.resolved.night_light, "the night light starts off");
	// A headless output has no gamma ramp, so nothing it shows can carry the
	// filter: what the frames look like is a check for the real backend (the
	// plan's on-screen box).
	check(!NightLight::holds_a_ramp(output()), "the headless output holds no gamma ramp");
	return after(1200);
}

Result shot_plain() {
	shot("plain");
	return after(300);
}

Result turn_on() {
	std::optional<Image> frame = read_shot("plain");
	check(frame.has_value(), "the plain frame was read");
	if (frame.has_value()) {
		plain = frame->average_rgb();
	}
	set_session("{\"nightLight\":true}");
	return after(1200);
}

Result night_light_in_conf() {
	if (auto wait = wait_for(server->conf.resolved.night_light)) {
		return *wait;
	}
	check(server->conf.resolved.night_light, "the night light is on in canvas.conf");
	check(server->conf.resolved.night_light_k == settings::night_light_temperature.fallback,
		"at the default temperature");
	float want[3];
	night_light_multipliers(settings::night_light_temperature.fallback, want);
	check(want[2] < 0.8 && want[0] > 0.99, "4000 K takes blue down and leaves red alone");
	float warmer[3];
	night_light_multipliers(2000, warmer);
	check(warmer[2] < want[2], "2000 K is warmer than 4000 K");
	server->damage_all();
	return after(800);
}

Result shot_filter_on() {
	shot("filter-on");
	return after(300);
}

// An output with no ramp is left as it was.
Result no_ramp_leaves_the_frame() {
	std::optional<Image> frame = read_shot("filter-on");
	check(frame.has_value(), "the frame was read with the filter on");
	if (frame.has_value()) {
		check_near(frame->average_rgb()[2], plain[2], 0.02,
			"an output with no gamma ramp shows the frame unchanged");
	}
	set_session("{\"nightLightTemperature\":2000}");
	return after(1200);
}

Result temperature_written() {
	if (auto wait = wait_for(server->conf.resolved.night_light_k == 2000)) {
		return *wait;
	}
	check(server->conf.resolved.night_light_k == 2000, "the temperature is written to canvas.conf");
	set_session("{\"nightLightTemperature\":99}");
	return after(1000);
}

Result temperature_out_of_range() {
	check(server->conf.resolved.night_light_k == 2000,
		"a temperature outside the range changes nothing");
	chord(KEY_N, mods::super_ctrl);
	return after(1200);
}

Result toggle_key_turns_it_off() {
	if (auto wait = wait_for(!server->conf.resolved.night_light)) {
		return *wait;
	}
	check(!server->conf.resolved.night_light, "the toggle key turned the night light off");
	chord(KEY_N, mods::super_ctrl);
	return after(1200);
}

Result toggle_key_turns_it_on() {
	if (auto wait = wait_for(server->conf.resolved.night_light)) {
		return *wait;
	}
	check(server->conf.resolved.night_light, "and the next press turned it back on");
	check(gamma.start_client("CANVAS_GAMMA_CLIENT"), "a gamma-control client started");
	return after(1500);
}

// The protocol is offered; this output has no ramp to give.
Result no_ramp_tells_the_client() {
	std::string_view said = gamma.said();
	check(!said.empty(), "the gamma-control client was answered");
	check(said.find("FAILED") != std::string_view::npos, std::format(
		"an output with no ramp tells a client so ({})",
		said.empty() ? "nothing said" : said.substr(0, said.find('\n'))));
	gamma.signal(SIGUSR1);
	return after(1200);
}

Result client_gone() {
	gamma.stop();
	check(server->conf.resolved.night_light,
		"the built-in setting is untouched by a client coming and going");
	return after(300);
}

// ------------------------------------------------ an output with a ramp

// The headless output gains a ramp: the built-in filter reaches it.
Result output_gains_a_ramp() {
	give_a_ramp(output().wlr);
	check(NightLight::holds_a_ramp(output()), "the output now holds a gamma ramp");
	ramp_commits.clear();
	server->damage_all();
	return after(600);
}

Result built_in_filter_committed() {
	check(server->conf.resolved.night_light, "the night light is still on");
	check(ramp_commits.size() == 1 && ramp_commits.back() != nullptr,
		std::format("the built-in filter is committed once with the ramp ({} commits)",
			ramp_commits.size()));
	server->damage_all();
	return after(400);
}

Result nothing_new_commits_nothing() {
	check(ramp_commits.size() == 1,
		std::format("frames with nothing new commit no transform ({} commits)",
			ramp_commits.size()));
	check(gamma.start_client("CANVAS_GAMMA_CLIENT"), "a gamma-control client started");
	return after(1500);
}

// A client's table replaces the filter on its output.
Result client_table_taken() {
	std::string_view said = gamma.said();
	check(said.find(std::format("SIZE {}", ramp_size)) != std::string_view::npos
			&& said.find("SET 1") != std::string_view::npos
			&& said.find("FAILED") == std::string_view::npos,
		std::format("the client was given the ramp's size and its table taken ({})", said));
	server->damage_all();
	return after(600);
}

Result client_table_committed() {
	check(ramp_commits.size() == 2 && ramp_commits.back() != nullptr,
		std::format("the client's table is committed once ({} commits)", ramp_commits.size()));
	set_session("{\"nightLight\":false}");
	return after(1200);
}

// The built-in filter is off, but the client's table is still shown.
Result client_keeps_the_output() {
	if (auto wait = wait_for(!server->conf.resolved.night_light)) {
		return *wait;
	}
	check(!server->conf.resolved.night_light, "the night light is off in canvas.conf");
	check(ramp_commits.size() == 2,
		std::format("a client holding the output keeps it ({} commits)", ramp_commits.size()));
	gamma.signal(SIGUSR1);
	return after(1200);
}

Result client_lets_go() {
	server->damage_all();
	return after(600);
}

// The client let go: the compositor's own state, which is none.
Result no_transform_shown() {
	check(ramp_commits.size() == 3 && ramp_commits.back() == nullptr,
		std::format("with the client gone and the filter off, no transform is shown"
			" ({} commits)", ramp_commits.size()));
	gamma.stop();
	refuse_tables = true;
	check(gamma.start_client("CANVAS_GAMMA_CLIENT"), "a second gamma-control client started");
	return after(1500);
}

// A table the output refuses: the client is told, nothing is committed.
Result refused_table() {
	std::string_view said = gamma.said();
	check(said.find("FAILED") != std::string_view::npos,
		std::format("a table the output cannot hold is refused ({})", said));
	refuse_tables = false;
	server->damage_all();
	return after(600);
}

Result refused_table_never_committed() {
	check(ramp_commits.size() == 3,
		std::format("a refused table is never committed ({} commits)", ramp_commits.size()));
	gamma.stop();
	return finish();
}

} // namespace

int main() {
	if (!begin("nightlight", {"WAYLAND_CANVAS_BIN", "CANVAS_GAMMA_CLIENT"})) {
		return 2;
	}
	Options options;
	options.start_bar = false;
	return run(options, {
		{"window on the output", window_on_the_output},
		{"night light starts off", night_light_starts_off},
		{"shot plain", shot_plain},
		{"turn on", turn_on},
		{"night light in conf", night_light_in_conf},
		{"shot filter on", shot_filter_on},
		{"no ramp leaves the frame", no_ramp_leaves_the_frame},
		{"temperature written", temperature_written},
		{"temperature out of range", temperature_out_of_range},
		{"toggle key turns it off", toggle_key_turns_it_off},
		{"toggle key turns it on", toggle_key_turns_it_on},
		{"no ramp tells the client", no_ramp_tells_the_client},
		{"client gone", client_gone},
		{"output gains a ramp", output_gains_a_ramp},
		{"built-in filter committed", built_in_filter_committed},
		{"nothing new commits nothing", nothing_new_commits_nothing},
		{"client table taken", client_table_taken},
		{"client table committed", client_table_committed},
		{"client keeps the output", client_keeps_the_output},
		{"client lets go", client_lets_go},
		{"no transform shown", no_transform_shown},
		{"refused table", refused_table},
		{"refused table never committed", refused_table_never_committed},
	});
}
