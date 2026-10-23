// The bar comes up with the compositor: the isolation proof, the first shot, the indicators found.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr int history_panel = 310; // the notifications bell's panel
constexpr int audio_panel = 280;

constexpr int tray_panel = 200;

// The first executable regular file called `name` along a PATH value, as
// a shell resolves a command by name; "" when none.
std::string path_lookup(std::string_view path, const std::string &name) {
	std::size_t start = 0;
	while (start <= path.size()) {
		std::size_t end = path.find(':', start);
		std::string_view entry = path.substr(start,
			end == std::string_view::npos ? std::string_view::npos : end - start);
		std::string candidate = std::format("{}/{}", entry.empty() ? "." : entry, name);
		struct stat st{};
		if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)
				&& access(candidate.c_str(), X_OK) == 0) {
			return candidate;
		}
		if (end == std::string_view::npos) {
			break;
		}
		start = end + 1;
	}
	return {};
}

} // namespace

std::vector<harness::Step> bringup_steps() {
	return {
		{"bringup_attach_devices", [] {
			harness::attach_pointer();
			harness::attach_keyboard();
			watch_bar_surfaces();
			return after(300);
		}},
		{"bringup_isolation_proof", [] {
			if (auto wait = wait_for(bar_layer() != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(bar_layer() != nullptr, "the bar client mapped its layer surface");
			// The isolation proof: what the bar process itself carries. A
			// journal check after the run (`journalctl -u NetworkManager -u
			// bluetooth --since <run start>`) confirms no bar traffic reached
			// the machine's services.
			pid_t bar_pid = 0;
			if (bar_client() != nullptr) {
				wl_client_get_credentials(bar_client(), &bar_pid, nullptr, nullptr);
			}
			std::string sys = proc_env_value(bar_pid, "DBUS_SYSTEM_BUS_ADDRESS");
			std::string ses = proc_env_value(bar_pid, "DBUS_SESSION_BUS_ADDRESS");
			std::println("note: the bar (pid {}) runs with DBUS_SYSTEM_BUS_ADDRESS={} "
				"DBUS_SESSION_BUS_ADDRESS={}", bar_pid, sys, ses);
			check(sys == private_system.address && ses == private_session.address,
				"the bar's bus addresses are the private pair (from /proc/environ)");
			// The shell is started with the compositor's own directory first on
			// its PATH: the scenario runs the compositor in-process, so that is
			// this executable's directory, and its helpers sit in it.
			std::string shell_path = proc_env_value(bar_pid, "PATH");
			std::error_code self_error;
			std::string self_dir = std::filesystem::read_symlink("/proc/self/exe",
				self_error).parent_path().string();
			std::println("note: the bar (pid {}) runs with PATH={}", bar_pid, shell_path);
			check(!self_error && !self_dir.empty() && shell_path.starts_with(self_dir + ":"),
				std::format("the shell's PATH starts with the compositor's own directory "
					"({})", self_dir));
			// The agents widget runs its collector by name: the first match on
			// that PATH is the build's copy, the one the scenario runs itself.
			// The scenario's own PATH holds no such directory, so only the
			// compositor's prefix can supply it.
			std::string resolved = path_lookup(shell_path, "wayland-canvas-agent-usage");
			const char *built = std::getenv("WAYLAND_CANVAS_AGENT_USAGE");
			std::error_code same_error;
			check(!resolved.empty() && built != nullptr
					&& std::filesystem::equivalent(resolved, built, same_error),
				std::format("wayland-canvas-agent-usage, run by name from the shell's "
					"PATH, is the build's ({})", resolved));
			return after(0);
		}},
		{"bringup_every_indicator_of_this_machine", [] {
			// every indicator of this machine, the clock and the weather
			// included, is on the bar once it has laid its widgets out
			if (auto wait = shot_holds("bar", [](cairo_surface_t *s) {
					LayerSurface *bar = bar_layer();
					if (bar == nullptr) {
						return false;
					}
					std::vector<Run> runs = bar_runs(s, bar->geo);
					long centred = std::ranges::count_if(runs, [](const Run &run) {
						return std::abs(run.centre() - screen_w / 2.0) <= 250;
					});
					// the logo, the theme switch, the clock, the weather and six indicators
					return runs.size() >= 4 + 6 && centred >= 2;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("bar");
			LayerSurface *layer = bar_layer();
			std::vector<Run> runs = s != nullptr && layer != nullptr
				? bar_runs(s, layer->geo) : std::vector<Run>{};
			cairo_surface_destroy(s);

			// Which indicators the bar shows, right to left, in the order the
			// right section lays them out. The stand-ins' fixed world carries a
			// battery, a Wi-Fi device, a Bluetooth adapter, a backlight and a
			// sink.
			std::vector<Indicator> mandatory;
			mandatory.push_back({"power", power_panel});
			// The notifications bell sits between the backlight and the
			// battery; its history panel is always there, like the bell.
			mandatory.push_back({"history", history_panel});
			mandatory.push_back({"brightness", brightness_panel});
			mandatory.push_back({"audio", audio_panel});
			mandatory.push_back({"network", network_panel});
			mandatory.push_back({"bluetooth", bluetooth_panel});

			bar_run_count = runs.size();
			// The logo is the leftmost run. drop it before matching the rest.
			if (!runs.empty()) {
				logo_x = runs.front().centre();
				runs.erase(runs.begin());
			}
			// The theme switch (canvas.theme) is the right-most widget of all:
			// record it for the theme phase and take it out before matching the
			// indicators, which are the right-most runs of what is left.
			if (!runs.empty()) {
				theme_x = runs.back().centre();
				runs.pop_back();
			}
			check(runs.size() >= 2 + mandatory.size(),
				std::format("every indicator of this machine is on the bar "
					"({} runs past the logo and the theme switch; the clock, the "
					"weather and {} indicators)", runs.size(), mandatory.size()));
			if (runs.size() < 2 + mandatory.size()) {
				return end_run("the bar's runs are not all there, so no position "
					"can be trusted");
			}

			// The right section's indicators are the rightmost runs: `mandatory`
			// lists them right to left, so the i-th counts from the right edge.
			std::size_t keep = runs.size() - mandatory.size();
			std::size_t first_mandatory = 0;
			for (std::size_t i = 0; i < mandatory.size(); i++) {
				Indicator &ind = mandatory[i];
				ind.x = runs[runs.size() - 1 - i].centre();
				if (std::strcmp(ind.name, "network") == 0) {
					ind.connect_flow = true;
					network_x = ind.x;
				}
				if (std::strcmp(ind.name, "bluetooth") == 0) {
					ind.bt_flow = true;
					bt_x = ind.x; // right-anchored: the agents and settings phases find the widget by it
				}
				if (std::strcmp(ind.name, "power") == 0) {
					ind.profile_flow = true;
					power_x = ind.x; // right-anchored: stable across the whole run
				}
			}

			// Between logo and the indicators: the tray items (right-click
			// menus), then the centred clock and weather (left to right).
			std::vector<double> centre;
			for (std::size_t i = 0; i < keep; i++) {
				double x = runs[i].centre();
				if (std::abs(x - screen_w / 2.0) <= 250) {
					centre.push_back(x);
				} else {
					indicators.push_back({"tray", tray_panel, x, BTN_RIGHT, close_outside});
				}
			}
			check(!centre.empty(), "the clock widget is on the bar");
			std::size_t clock_index = indicators.size();
			if (!centre.empty()) {
				clock_x = centre.front();
				indicators.push_back({"clock", clock_panel, clock_x, BTN_LEFT, close_outside});
				// The canned fetch always answers, so the widget is always
				// there: its panel is checked like any other, its rows read
				// against the fixture's days.
				check(centre.size() >= 2, "the weather widget is on the bar "
					"(the canned fetch decided at once)");
				if (centre.size() >= 2) {
					indicators.push_back({"weather", weather_panel, centre.back()});
					indicators.back().wx_flow = true;
				}
			}
			first_mandatory = indicators.size();
			for (const Indicator &ind : mandatory) {
				indicators.push_back(ind);
			}

			// Spread the close styles over panels that exist: the rightmost
			// indicator proves the second-click close, another proves Escape.
			if (!mandatory.empty()) {
				indicators[first_mandatory].close_mode = close_icon;
			}
			bool escape_set = false;
			if (mandatory.size() > 1) {
				indicators.back().close_mode = close_escape;
				escape_set = true;
			}
			if (!escape_set && clock_index < indicators.size()) {
				indicators[clock_index].close_mode = close_escape;
			}

			check(network_x > 0, "the network indicator is present (the typing "
				"check needs its panel)");
			return after(300);
		}},
	};
}

} // namespace bar
