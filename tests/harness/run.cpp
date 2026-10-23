#include "harness/run.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include "harness/check.h"
#include "harness/child.h"
#include "harness/control.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "platform/output.h"
#include "windows/view.h"

namespace harness {

std::optional<Server> server;

namespace {

constexpr int first_tick_ms = 200;
constexpr int shot_poll_ms = 30;
constexpr uint32_t step_clock_ms = 1000;

wl_event_source *timer = nullptr;

int on_tick() {
	// grim runs beside the compositor: the step after a shot waits for it.
	if (shot_running()) {
		wl_event_source_timer_update(timer, shot_poll_ms);
		return 0;
	}
	advance_clock(step_clock_ms);
	Sequencer::Tick tick = sequencer().tick();
	if (tick.done) {
		wl_display_terminate(server->display);
		return 0;
	}
	wl_event_source_timer_update(timer, tick.delay_ms);
	return 0;
}

} // namespace

bool begin(std::string_view scenario, std::initializer_list<const char *> required_env) {
	wlr_log_init(WLR_ERROR, nullptr);
	for (const char *name : required_env) {
		const char *value = std::getenv(name);
		if (value == nullptr || *value == '\0') {
			std::fprintf(stderr, "%.*s: $%s is not set (meson passes it)\n",
				static_cast<int>(scenario.size()), scenario.data(), name);
			return false;
		}
	}
	make_scratch(scenario);
	use_headless_backend();
	// The compositor's own settings and config never come from the user's.
	scratch_env("XDG_CONFIG_HOME", "xdg-config");
	return true;
}

void use_headless_backend() {
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
}

std::string scratch_env(const char *name, std::string_view relative) {
	std::string path = scratch_dir(relative);
	setenv(name, path.c_str(), 1);
	return path;
}

void put_first_on_path(const std::string &dir) {
	const char *path = std::getenv("PATH");
	setenv("PATH", std::format("{}:{}", dir, path != nullptr ? path : "").c_str(), 1);
}

int run(const Options &options, std::vector<Step> steps, std::function<void()> teardown) {
	server.emplace();
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	Sequencer sequence(std::move(steps));
	timer = wl_event_loop_add_timer(server->loop, [](void *) { return on_tick(); }, nullptr);
	wl_event_source_timer_update(timer, first_tick_ms);
	wl_display_run(server->display);
	if (teardown) {
		teardown();
	}
	if (!sequence.error().empty()) {
		check(false, sequence.error());
	} else if (!sequence.finished()) {
		check(false, std::format("the run ended at step {}, before its last step",
			sequence.current()));
	}
	stop_control_clients();
	stop_children();
	wl_event_source_remove(timer);
	detach_devices();
	server.reset();
	return failures() != 0;
}

wlr_output *add_output(int width, int height) {
	wlr_backend *headless = nullptr;
	wlr_multi_for_each_backend(server->backend,
		[](wlr_backend *backend, void *found) {
			if (wlr_backend_is_headless(backend)) {
				*static_cast<wlr_backend **>(found) = backend;
			}
		}, &headless);
	if (headless == nullptr) {
		return nullptr;
	}
	return wlr_headless_add_output(headless, static_cast<unsigned>(width),
		static_cast<unsigned>(height));
}

Output &output() {
	return *server->outputs.front();
}

bool settled(const Output &output) {
	return !output.camera.panning() && !output.camera.zooming() && output.frames_alpha == 0;
}

bool settled() {
	for (const std::unique_ptr<Output> &each : server->outputs) {
		if (!settled(*each)) {
			return false;
		}
	}
	return true;
}

View *first_mapped_view() {
	for (View *view : server->views) {
		if (view->mapped) {
			return view;
		}
	}
	return nullptr;
}

} // namespace harness
