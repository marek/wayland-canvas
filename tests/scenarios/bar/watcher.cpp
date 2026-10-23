// The config file's watcher: how the shell hears about shell.json, and a broken file.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

std::string config_good;   // the scratch shell.json before the edit that breaks it
std::string config_broken; // and as that edit leaves it: cut short, not JSON

double watch_edge = 0; // the left section's ink edge as the last read of it found it

// The left section's ink edge on a fresh shot of `name`; false while the
// shot is still landing. 0 in `edge` means the bar could not be read (no
// bar mapped, or a reload between its surfaces).
bool watch_read(const char *name, bool light, double &edge) {
	if (!shot_landed(name)) {
		return false;
	}
	LayerSurface *bar = bar_layer();
	cairo_surface_t *s = load_png(name);
	edge = bar != nullptr && s != nullptr ? bar_ink_edge(s, bar->geo, light) : 0;
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return true;
}

int watch_retries = 0;

// Settles one save of shell.json: the hello plugin's label moves the left section's edge
// right when an edit turns it on and left when one turns it off, and a reload replaces the
// bar's surface. False while undecided; true once the verdict is recorded.
bool watch_settle(const char *name, bool wider, bool light,
	std::string_view what) {
	double edge = 0;
	if (!watch_read(name, light, edge)) {
		return false;
	}
	bool moved = edge > 0 && (wider ? edge >= watch_edge + widen_px
					: edge <= watch_edge - widen_px);
	if (!moved && watch_retries++ < 60) { // a reload recompiles every plugin: in flight under load
		return false;
	}
	watch_retries = 0;
	check(moved, std::format("{} applied: the hello label {} the left "
		"section's edge ({} px, was {})", what, wider ? "pushed" : "released",
		edge, watch_edge));
	check(!settings_shell_kept(), std::format("{} reloaded the shell (the "
		"bar is a new surface)", what));
	watch_edge = edge;
	watch_remember_bar();
	return true;
}

} // namespace

std::string with_hello(bool on) {
	std::string text = read_file(shell_config);
	std::string from = on ? "\"enabled\": false" : "\"enabled\": true";
	std::string to = on ? "\"enabled\": true" : "\"enabled\": false";
	std::size_t at = text.find(from);
	if (at != std::string::npos) {
		text.replace(at, from.size(), to);
	}
	return text;
}

void watch_remember_bar() {
	bar_surfaces_remembered = bar_surfaces_made;
}

std::vector<harness::Step> watcher_steps() {
	return {
		hello_plugin_step("watcher_hello_plugin_installed"),
		{"watcher_baseline", [] {
			double edge = 0;
			if (!watch_read("watch-base", false, edge)) {
				return again(300);
			}
			if (auto wait = wait_for(edge > 0, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(edge > 0, std::format("the bar is readable before the config "
				"edits (the left section's edge at {} px)", edge));
			watch_edge = edge;
			watch_remember_bar();
			replace_file(shell_config, with_hello(true));
			return after(600);
		}},
		{"watcher_first_rename_over_reloaded", [] {
			if (!watch_settle("watch-rename-1", true, false,
					"the first save of shell.json by rename-over")) {
				return again(400);
			}
			replace_file(shell_config, with_hello(false));
			return after(600);
		}},
		{"watcher_second_rename_over_reloaded_too", [] {
			if (!watch_settle("watch-rename-2", false, false,
					"the second save of shell.json by rename-over")) {
				return again(400);
			}
			std::error_code ec;
			std::filesystem::remove(shell_config, ec);
			check(!ec && !std::filesystem::exists(shell_config),
				"shell.json is deleted");
			return after(600);
		}},
		{"watcher_deletion_reloaded_on_the_defaults", [] {
			if (!watch_settle("watch-deleted", true, false,
					"deleting shell.json")) {
				return again(400);
			}
			check(!std::filesystem::exists(shell_config), "the reloaded shell "
				"wrote nothing back: shell.json is still missing");
			write_file(shell_config, hello_off_config); // hello off again
			return after(600);
		}},
		{"watcher_creation_reloaded", [] {
			if (!watch_settle("watch-created", false, false,
					"creating shell.json by hand")) {
				return again(400);
			}
			click(theme_x, bar_mid_y()); // the switch: dark -> light, a host write
			return after(500);
		}},
		{"watcher_hosts_own_write_landed", [] {
			bool written = read_file(shell_config).find("\"mode\": \"light\"")
				!= std::string::npos;
			if (auto wait = wait_for(written, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(written, "the switch wrote theme.mode: light into the created "
				"shell.json");
			return after(700); // long enough for a reload to show if the write caused one
		}},
		{"watcher_it_reloaded_nothing", [] {
			check(settings_shell_kept(), "the host's own write of shell.json "
				"reloaded nothing");
			replace_file(shell_config, with_hello(true));
			return after(600);
		}},
		{"watcher_save_after_the_hosts_write", [] {
			if (!watch_settle("watch-after-write", true, true,
					"a rename-over after the host's own write")) {
				return again(400);
			}
			click(theme_x, bar_mid_y()); // light -> dark, a host write again
			return after(500);
		}},
		{"watcher_switch_wrote_theme_mode", [] {
			bool written = read_file(shell_config).find("\"mode\": \"dark\"")
				!= std::string::npos;
			if (auto wait = wait_for(written, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(written, "the switch wrote theme.mode: dark back");
			return after(700);
		}},
		{"watcher_nothing_reloaded", [] {
			check(settings_shell_kept(), "the host's second write of shell.json "
				"reloaded nothing");
			replace_file(shell_config, with_hello(false));
			return after(600);
		}},
		{"watcher_last_reload", [] {
			if (!watch_settle("watch-last", false, false,
					"the rename-over after the host's second write")) {
				return again(400);
			}
			return after(100);
		}},
		{"watcher_shell_json_cut_short_as", [] {
			config_good = read_file(shell_config);
			check(config_good.size() > 8, "the scratch shell.json is there to break");
			// Half of it: an object never closed, ending on a character
			// read_file keeps, so the file reads back as written.
			config_broken = config_good.substr(0, config_good.size() / 2);
			while (!config_broken.empty() && std::isspace(static_cast<unsigned char>(config_broken.back()))) {
				config_broken.pop_back();
			}
			watch_remember_bar();
			replace_file(shell_config, config_broken);
			return after(700);
		}},
		{"watcher_shell_reloaded_on_that_edit", [] {
			if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(shell_reloaded(), "the shell reloaded on the edit that left shell.json unparsable");
			watch_remember_bar();
			check(read_file(shell_config) == config_broken,
				"the shell started on shell.json that does not parse and left it as it was");
			return after(1500); // its start tells the compositor its mode: let that land first
		}},
		{"watcher_outside_theme_set", [] {
			outside_theme_set("light");
			return after(800);
		}},
		{"watcher_outside_theme_set_switched_the", [] {
			bool light = server->theme.mode() == ThemeMode::light;
			if (auto wait = wait_for(light, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(light, "the outside theme.set switched the compositor to light");
			shot("config-broken-bar");
			return after(700);
		}},
		{"watcher_adopted_in_memory", [] {
			LayerSurface *l = bar_layer();
			bool adopted = l != nullptr
				&& shot_pixel_is("config-broken-bar", l->geo.x + 3, l->geo.y + l->geo.height / 2, shell_bg_light);
			if (auto wait = wait_for(adopted, {.poll_ms = 700, .budget_ms = 28000})) {
				shot("config-broken-bar");
				return *wait;
			}
			check(adopted, "the shell applied the switch in memory (the bar is #e1e2e7)");
			check(read_file(shell_config) == config_broken,
				"the theme change left the shell.json that does not parse byte-identical");
			watch_remember_bar();
			replace_file(shell_config, config_good); // fixed: it parses again
			return after(700);
		}},
		{"watcher_fixed_file_reloaded_the_shell", [] {
			if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(shell_reloaded(), "the shell reloaded once shell.json parsed again");
			watch_remember_bar();
			return after(1500);
		}},
		{"watcher_flip_to_light_again", [] {
			outside_theme_set("light");
			return after(500);
		}},
		{"watcher_once_shell_json_parsed_again", [] {
			bool written = read_file(shell_config).find("\"mode\": \"light\"") != std::string::npos;
			if (auto wait = wait_for(written, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(written, "once shell.json parsed again, the next theme flip wrote theme.mode: light into it");
			outside_theme_set("dark");
			return after(500);
		}},
		{"watcher_and_the_flip_back_wrote", [] {
			bool written = read_file(shell_config).find("\"mode\": \"dark\"") != std::string::npos;
			if (auto wait = wait_for(written, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(written, "and the flip back wrote theme.mode: dark");
			return after(200);
		}},
	};
}

} // namespace bar
