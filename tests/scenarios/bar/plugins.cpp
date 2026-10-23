// The plugin world: a dropped-in, edited and disabled user plugin, and the idle watch.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

std::size_t base_runs = 0; // the bar's run count before the user plugin

double user_x1 = 0; // the user widget's right edge, before the QML edit

int idle_ticks = 0; // samples left of the idle watch
bool idle_spawned = false; // any child of the shell seen while idle
constexpr int idle_sample_count = 1000; // 10 s of samples
constexpr int idle_sample_ms = 10;

constexpr std::string_view hello_label_qml =
	"import QtQuick\nimport WaylandCanvas.Ui\nBarButton {\n"
	"\ttext: \"HELLO SHELL\"\n}\n";

bool hello_installing = false; // hello_plugin_step has dropped the plugin in and awaits its reload

// Whether a live process has any child right now, as /proc tells it —
// the idle watch: a shell that reloads by change notifications must
// spawn nothing while it just sits there.
bool proc_has_children(pid_t pid) {
	if (pid <= 0) {
		return false;
	}
	std::string tasks = std::format("/proc/{}/task", pid);
	DIR *dir = opendir(tasks.c_str());
	if (dir == nullptr) {
		return false;
	}
	bool any = false;
	struct dirent *e = nullptr;
	while (!any && (e = readdir(dir)) != nullptr) {
		if (e->d_name[0] == '.') {
			continue;
		}
		FILE *f = std::fopen((tasks + "/" + e->d_name + "/children").c_str(), "r");
		if (f == nullptr) {
			continue;
		}
		char buf[512];
		while (!any && std::fgets(buf, sizeof buf, f) != nullptr) {
			for (const char *p = buf; *p != '\0'; p++) {
				if (*p != ' ' && *p != '\n') {
					any = true;
					break;
				}
			}
		}
		std::fclose(f);
	}
	closedir(dir);
	return any;
}

} // namespace

const std::string_view hello_off_config =
	"{\n\t\"plugins\": {\n\t\t\"canvas.hello\": {\n\t\t\t\"enabled\": false\n"
	"\t\t}\n\t}\n}\n";

harness::Step hello_plugin_step(std::string_view name) {
	return {name, [] {
		if (!hello_installing) {
			if (std::filesystem::exists(user_plugin + "/plugin.json")) {
				return after(0);
			}
			watch_remember_bar();
			write_file(shell_config, hello_off_config);
			write_file(staged_plugin + "/Hello.qml", hello_label_qml);
			std::error_code moved;
			std::filesystem::rename(staged_plugin, user_plugin, moved); // one change for the shell to see
			check(!moved, std::format("the hello plugin was installed into the data dir ({})",
				moved.message()));
			hello_installing = true;
		}
		if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 300, .budget_ms = 60000})) {
			return *wait;
		}
		hello_installing = false;
		return after(500);
	}};
}

std::vector<harness::Step> plugins_steps() {
	return {
		{"plugins_every_run_on_the_bar", [] {
			// every run on the bar, accounted for, so the broken manifest's plugin
			// contributed nothing; base_runs counts the theme switch too, which the
			// later reload checks compare against
			constexpr std::size_t mandatory = 6;
			struct Account {
				std::size_t runs, centre, keep;
			};
			auto account = [](cairo_surface_t *s) {
				LayerSurface *layer = bar_layer();
				std::vector<Run> runs = s != nullptr && layer != nullptr
					? bar_runs(s, layer->geo) : std::vector<Run>{};
				std::size_t total = runs.size();
				if (!runs.empty()) { // the theme switch, right-most of all
					runs.pop_back();
				}
				std::size_t keep = runs.size() >= 1 + mandatory ? runs.size() - 1 - mandatory : 0;
				std::size_t centre = 0;
				for (std::size_t i = 1; i < 1 + keep; i++) {
					if (std::abs(runs[i].centre() - screen_w / 2.0) <= 250) {
						centre++;
					}
				}
				return Account{total, centre, keep};
			};
			if (auto wait = shot_holds("bar-plain", [&](cairo_surface_t *s) {
					Account seen = account(s);
					return seen.keep - seen.centre == 1;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("bar-plain");
			LayerSurface *layer = bar_layer();
			Account seen = account(s);
			cairo_surface_destroy(s);
			check(layer != nullptr && seen.runs >= 1, "the bar is still mapped after the walk");
			base_runs = seen.runs;
			check(seen.keep - seen.centre == 1, std::format("every run on the bar is accounted for "
				"({} runs: logo, {} centred, 1 tray, {} indicators, the theme switch "
				"— the broken manifest's plugin was skipped)", base_runs, seen.centre, mandatory));
			// Drop the user plugin's staged files into the scratch data dir: the
			// shell reloads with them, without anything of the shell being touched.
			mkdir_p(user_plugin);
			std::rename((staged_plugin + "/plugin.json").c_str(),
				(user_plugin + "/plugin.json").c_str());
			std::rename((staged_plugin + "/Hello.qml").c_str(),
				(user_plugin + "/Hello.qml").c_str());
			return after(500);
		}},
		{"plugins_shell_reloads_with_the_dropped", [] {
			shot("bar-user");
			return after(700);
		}},
		{"plugins_user_plugins_bar_widget_appeared", [] {
			cairo_surface_t *s = load_png("bar-user");
			LayerSurface *layer = bar_layer();
			std::vector<Run> runs = s != nullptr && layer != nullptr
				? bar_runs(s, layer->geo) : std::vector<Run>{};
			cairo_surface_destroy(s);
			bool appeared = runs.size() == base_runs + 1 && runs.size() >= 2
				&& runs[1].x1 < 300; // the new run sits beside the logo, left of centre
			// A reload recompiles every plugin and remakes the bar, under load
			// slowly: re-shoot and look again until the widget shows.
			if (auto wait = wait_for(appeared, {.poll_ms = 700, .budget_ms = 105000})) {
				shot("bar-user");
				return *wait;
			}
			check(appeared, std::format("the user plugin's bar widget appeared after it "
				"was dropped into the data dir ({} runs, was {})", runs.size(), base_runs));
			user_x1 = runs.size() >= 2 ? runs[1].x1 : 0;
			// Edit the loaded plugin's own QML while the shell runs: the
			// shell reloads with it — the widget becomes a labelled one.
			write_file(user_plugin + "/Hello.qml", hello_label_qml);
			return after(500);
		}},
		{"plugins_shell_reloads_with_the_edited", [] {
			shot("bar-wide");
			return after(700);
		}},
		{"plugins_widget_may_now_merge_into", [] {
			cairo_surface_t *s = load_png("bar-wide");
			LayerSurface *layer = bar_layer();
			std::vector<Run> runs = s != nullptr && layer != nullptr
				? bar_runs(s, layer->geo) : std::vector<Run>{};
			cairo_surface_destroy(s);
			// The widget may now merge into the logo's run, so what is
			// measured is the left section's right edge — the rightmost lit
			// pixel short of the centred widgets — which the wider widget
			// must have pushed right.
			double left_right = 0;
			for (std::size_t i = 0; i < runs.size(); i++) {
				if (runs[i].x1 < screen_w / 2.0 - 250 && runs[i].x1 > left_right) {
					left_right = runs[i].x1;
				}
			}
			bool widened = layer != nullptr && left_right >= user_x1 + widen_px;
			// the reload is in flight under load
			if (auto wait = wait_for(widened, {.poll_ms = 700, .budget_ms = 105000})) {
				shot("bar-wide");
				return *wait;
			}
			check(widened, std::format("editing the loaded plugin's QML file while the "
				"shell runs changed the bar (the left section's right edge {} to {} px)",
				user_x1, left_right));
			// Disable the plugin in the scratch config: the shell reloads
			// without it.
			write_file(shell_config, hello_off_config);
			return after(500);
		}},
		{"plugins_shell_reloads_again_without_the", [] {
			shot("bar-disabled");
			return after(700);
		}},
		{"plugins_disabling_the_user_plugin_in", [] {
			cairo_surface_t *s = load_png("bar-disabled");
			LayerSurface *layer = bar_layer();
			std::vector<Run> runs = s != nullptr && layer != nullptr
				? bar_runs(s, layer->geo) : std::vector<Run>{};
			cairo_surface_destroy(s);
			bool gone = runs.size() == base_runs
				&& (runs.size() < 2 || runs[1].x1 >= 300); // no run beside the logo
			// the reload is in flight under load
			if (auto wait = wait_for(gone, {.poll_ms = 700, .budget_ms = 105000})) {
				shot("bar-disabled");
				return *wait;
			}
			check(gone, std::format("disabling the user plugin in the config removed its "
				"widget ({} runs, was {} after the drop-in)", runs.size(), base_runs + 1));
			// Then the idle watch: the reloads above were change
			// notifications, so a shell that just sits there must spawn
			// nothing — no watcher poll, ever.
			bar_pid = 0;
			if (bar_client() != nullptr) {
				wl_client_get_credentials(bar_client(), &bar_pid, nullptr, nullptr);
			}
			idle_ticks = idle_sample_count;
			idle_spawned = false;
			return after(idle_sample_ms);
		}},
		{"plugins_every_few_milliseconds_one_look", [] {
			if (proc_has_children(bar_pid)) {
				idle_spawned = true;
			}
			if (idle_ticks-- > 0) {
				return again(idle_sample_ms);
			}
			check(!idle_spawned && bar_pid > 0, std::format("an idle shell spawned no "
				"processes ({} samples over {} s)", idle_sample_count,
				idle_sample_count * idle_sample_ms / 1000));
			return after(100);
		}},
	};
}

} // namespace bar
