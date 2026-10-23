// The bar scenario: the real compositor with the real bar client (qs) on private D-Bus buses
// of stand-in services (tests/scenarios/bar-standins.py), fake devices and grim. One file per
// phase, each a list of named steps (harness/steps.h); the phases named by the first argument
// or CANVAS_BAR_PHASE (comma separated) run after the bring-up, all when none is named, and
// `--phases a,b,...` runs nothing and fails on a name that is not a phase.
#include "scenarios/bar/world.h"

namespace {

struct Phase {
	std::string_view name;
	std::vector<harness::Step> (*steps)();
};

constexpr Phase phases[] = {
	{"bringup", bar::bringup_steps},
	{"walk", bar::walk_steps},
	{"plugins", bar::plugins_steps},
	{"overview", bar::overview_steps},
	{"toolbar", bar::toolbar_steps},
	{"theme", bar::theme_steps},
	{"watcher", bar::watcher_steps},
	{"nightlight", bar::nightlight_steps},
	{"wheel", bar::wheel_steps},
	{"notes", bar::notes_steps},
	{"network", bar::network_steps},
	{"agents", bar::agents_steps},
	{"recording", bar::recording_steps},
	{"edges", bar::edges_steps},
	{"barwrites", bar::barwrites_steps},
	{"socket", bar::socket_steps},
	{"settings", bar::settings_steps},
	{"appbar", bar::appbar_steps},
	{"plugin_settings", bar::plugin_settings_steps},
	{"appbar_hover", bar::appbar_hover_steps},
	{"screenshots", bar::screenshots_steps},
	{"shared", bar::shared_steps},
	{"handover", bar::handover_steps},
};

} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0); // PASS/FAIL lines must survive a kill
	auto names = [](std::string_view list) {
		std::vector<std::string_view> out;
		for (auto part : std::views::split(list, ',')) {
			if (!part.empty()) {
				out.emplace_back(part.begin(), part.end());
			}
		}
		return out;
	};
	if (argc > 2 && std::string_view(argv[1]) == "--phases") {
		std::vector<std::string_view> registered = names(argv[2]);
		bool same = registered.size() + 1 == std::size(phases);
		for (const Phase &phase : phases) {
			if (&phase != &phases[0] && !std::ranges::contains(registered, phase.name)) {
				std::println(stderr, "phase '{}' is not registered in meson.build", phase.name);
				same = false;
			}
		}
		return same ? 0 : 1;
	}
	const char *env = std::getenv("CANVAS_BAR_PHASE");
	std::string only = argc > 1 ? argv[1] : env != nullptr ? env : "";
	std::vector<std::string_view> wanted = names(only);
	for (std::string_view name : wanted) {
		if (std::ranges::none_of(phases, [name](const Phase &phase) { return phase.name == name; })) {
			std::println(stderr, "no bar phase called '{}'; the phases are:", name);
			for (const Phase &phase : phases) {
				std::println(stderr, "  {}", phase.name);
			}
			return 2;
		}
	}
	// Runs of different phases go side by side: each has its own scratch
	// folder and its own buses.
	std::string run_name = "bar";
	for (std::string_view name : wanted) {
		run_name += std::format("-{}", name);
	}
	bar::remember_machine_environment();
	if (!harness::begin(run_name, {})) {
		return 2;
	}
	std::vector<harness::Step> steps;
	for (const Phase &phase : phases) {
		bool is_first = &phase == &phases[0];
		if (wanted.empty() || is_first || std::ranges::contains(wanted, phase.name)) {
			std::vector<harness::Step> more = phase.steps();
			steps.insert(steps.end(), std::make_move_iterator(more.begin()),
				std::make_move_iterator(more.end()));
		}
	}
	if (!bar::bring_up(run_name)) {
		bar::stop_isolation();
		return 2;
	}
	Options options;
	int status = harness::run(options, std::move(steps), bar::tear_down);
	bar::stop_isolation();
	return status;
}
