// The theme switch: both palettes, the reload keeping the mode, an outside theme.set adopted.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

std::string scheme_start; // the machine's real color-scheme, read before the run

constexpr uint32_t shell_panel_light = 0xf4f5f8ff;

// The user's real color-scheme (org.gnome.desktop.interface), through a
// child with the machine's own environment restored — empty when there is
// no session bus to read it from, in which case the run cannot verify it.
std::string real_color_scheme() {
	int fds[2];
	if (pipe(fds) != 0) {
		return "";
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		for (SavedEnv &saved : saved_env) {
			if (saved.set) {
				setenv(saved.name, saved.value.c_str(), 1);
			} else {
				unsetenv(saved.name);
			}
		}
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		prctl(PR_SET_PDEATHSIG, SIGKILL); // a killed run must not leak it
		execlp("gsettings", "gsettings", "get", "org.gnome.desktop.interface",
			"color-scheme", static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	std::string out;
	char chunk[256];
	ssize_t n;
	while ((n = read(fds[0], chunk, sizeof chunk)) > 0) {
		out.append(chunk, static_cast<std::size_t>(n));
	}
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return "";
	}
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

// The target tab's fill pixel in a shot: the tab layout is read from the
// compositor itself, the sample point is the tab's clear top-left corner.
// A note in the log names the rect and pixel, so a failure says why.
bool target_tab_is(const char *name, uint32_t want) {
	Output *o = out();
	Desktop *d = o != nullptr
		? o->server.canvas.desktops.find(o->overview.target().desktop) : nullptr;
	std::optional<TabLayout> layout = d != nullptr
		? overview_tab_layout(*o, *d) : std::nullopt;
	cairo_surface_t *s = load_png(name);
	int x = layout.has_value() ? layout->tab.x + 4 : 0;
	int y = layout.has_value() ? layout->tab.y + 4 : 0;
	bool ok = layout.has_value() && pixel_is(s, x, y, want);
	std::println("note: tab sample ({},{}): {:06x}, want {:06x}, layout {}, desktop {}",
		x, y, s != nullptr ? pixel_at(s, x, y) : 0, want,
		layout.has_value() ? "found" : "missing", d != nullptr ? "found" : "missing");
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return ok;
}

// The themed window's title bar in a shot: one of the mode's two title
// colours (which one depends on focus; both follow the mode), sampled mid
// bar where no title or button is drawn. A note in the log names the
// sample point and pixel, so a failure says why.
bool title_bar_is(const char *name, uint32_t active, uint32_t inactive) {
	View *v = titled("themed");
	wlr_box f = v != nullptr ? v->frame_box() : wlr_box{};
	int x = f.x + f.width / 2, y = f.y + config::titlebar_height / 2;
	cairo_surface_t *s = load_png(name);
	bool ok = v != nullptr && s != nullptr
		&& (pixel_is(s, x, y, active) || pixel_is(s, x, y, inactive));
	std::println("note: title sample ({},{}): {:06x}, want {:06x} or {:06x}, view {}",
		x, y, s != nullptr ? pixel_at(s, x, y) : 0, active, inactive,
		v != nullptr ? "found" : "missing");
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return ok;
}

} // namespace

std::vector<harness::Step> theme_steps() {
	return {
		hello_plugin_step("theme_hello_plugin_installed"),
		themed_window_step("theme_themed_window"),
		{"theme_phase", [] {
			if (theme_x <= 0) {
				return end_run("the theme switch was not found on the bar");
			}
			return after(0);
		}},
		{"theme_shot_dark_window", [] {
			if (auto wait = wait_for(titled("themed") != nullptr, {.poll_ms = 300, .budget_ms = 30000})) {
				return *wait;
			}
			check(titled("themed") != nullptr, "the themed window mapped");
			shot("dark-window");
			return after(700);
		}},
		{"theme_title_bar_the_compositor_draws", [] {
			bool dark = title_bar_is("dark-window", config::palette_dark.title_active,
				config::palette_dark.title_inactive);
			if (auto wait = wait_for(dark, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("dark-window"); // re-shoot on this step: retries survive it
				return *wait;
			}
			check(dark, "the window's title bar is dark (a dark-mode title colour)");
			chord(KEY_TAB, mods::super); // the overview, with its tabs
			return after(400);
		}},
		{"theme_overview_opens", [] {
			Output *o = out();
			if (auto wait = wait_for(!(!(o != nullptr && o->overview.active())), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(o != nullptr && o->overview.active(), "Super+Tab opened the overview");
			if (Desktop *d = o->server.canvas.desktops.find(o->overview.target().desktop);
				d != nullptr) {
				o->overview.set_target(d->frame(), d->id(), false);
			}
			shot("dark-overview");
			return after(700);
		}},
		{"theme_target_tabs_fill_is_the", [] {
			bool accent = target_tab_is("dark-overview", config::palette_dark.accent);
			if (auto wait = wait_for(accent, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("dark-overview");
				return *wait;
			}
			check(accent, "the overview's target tab is the dark accent fill");
			tap(KEY_ESC);
			return after(400);
		}},
		{"theme_overview_closed", [] {
			if (auto wait = wait_for(!(out() != nullptr && out()->overview.active()), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(out() != nullptr && !out()->overview.active(), "the overview closed");
			click(theme_x, bar_mid_y()); // the switch: dark -> light
			return after(500);
		}},
		{"theme_switchs_two_promises", [] {
			std::string cfg = read_file(shell_config);
			bool written = cfg.find("\"mode\": \"light\"") != std::string::npos;
			bool told = server->theme.mode() == ThemeMode::light;
			if (auto wait = wait_for(!(!(written && told)), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(written, "clicking the switch wrote theme.mode: light to the scratch shell.json");
			check(told, "the compositor reports theme light (told over the control socket)");
			shot("light-bar");
			return after(700);
		}},
		{"theme_bar_itself_follows", [] {
			LayerSurface *l = bar_layer();
			bool light = l != nullptr
				&& shot_pixel_is("light-bar", l->geo.x + 3, l->geo.y + l->geo.height / 2, shell_bg_light);
			if (auto wait = wait_for(light, {.poll_ms = 700, .budget_ms = 28000})) {
				shot("light-bar");
				return *wait;
			}
			check(light, "the bar is light (its background pixel is #e1e2e7)");
			click(power_x, bar_mid_y()); // a panel, to see the light panel surface
			return after(400);
		}},
		{"theme_shot_light_panel", [] {
			if (auto wait = wait_for(panel_layer("power") != nullptr, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("power") != nullptr, "the power panel opened in light mode");
			shot("light-panel");
			return after(700);
		}},
		{"theme_panels_card_against_the_palette", [] {
			wlr_box card = card_box(power_x, power_panel);
			bool light = shot_pixel_is("light-panel", card.x + 5, card.y + 5, shell_panel_light);
			if (auto wait = wait_for(light, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("light-panel");
				return *wait;
			}
			check(light, "the open panel is light (its card is #f4f5f8)");
			click_outside_cards(); // outside: closed again
			return after(400);
		}},
		{"theme_light_panel_closed_on_a", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the light panel closed on a click outside");
			shot("light-window");
			return after(700);
		}},
		{"theme_title_bar_followed_too", [] {
			bool light = title_bar_is("light-window", config::palette_light.title_active,
				config::palette_light.title_inactive);
			if (auto wait = wait_for(light, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("light-window");
				return *wait;
			}
			check(light, "the window's title bar is light (a light-mode title colour)");
			chord(KEY_TAB, mods::super);
			return after(400);
		}},
		{"theme_overview_again_now_in_light", [] {
			Output *o = out();
			if (auto wait = wait_for(!(!(o != nullptr && o->overview.active())), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(o != nullptr && o->overview.active(), "the overview opened in light mode");
			if (Desktop *d = o->server.canvas.desktops.find(o->overview.target().desktop);
				d != nullptr) {
				o->overview.set_target(d->frame(), d->id(), false);
			}
			shot("light-overview");
			return after(700);
		}},
		{"theme_target_tabs_fill_is_the_2", [] {
			bool accent = target_tab_is("light-overview", config::palette_light.accent);
			if (auto wait = wait_for(accent, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("light-overview");
				return *wait;
			}
			check(accent, "the overview's target tab is the light accent fill");
			tap(KEY_ESC);
			return after(400);
		}},
		{"theme_reload_must_keep_the_mode", [] {
			if (auto wait = wait_for(!(out() != nullptr && out()->overview.active()), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(out() != nullptr && !out()->overview.active(), "the overview closed");
			// The bar as it is before the edit, for the before/after comparison.
			shot("light-prereload");
			return after(700);
		}},
		{"theme_re_enable_the_user_plugin", [] {
			// re-enabling the user plugin rewrites the config, so the shell reloads: the
			// widget coming back proves it, theme.mode: light proves the mode survived
			std::string cfg = read_file(shell_config);
			std::size_t at = cfg.find("\"enabled\": false");
			if (at != std::string::npos) {
				cfg.replace(at, 16, "\"enabled\": true");
			}
			write_file(shell_config, cfg);
			// The after picture, taken as the edit lands: the next step reads it
			// before anything else, and re-shoots it until the reload shows.
			shot("light-reloaded");
			return after(700);
		}},
		{"theme_reloads_two_proofs", [] {
			LayerSurface *l = bar_layer();
			cairo_surface_t *pre = load_png("light-prereload");
			cairo_surface_t *re = load_png("light-reloaded");
			double before = l != nullptr && pre != nullptr ? bar_ink_edge(pre, l->geo, true) : 0;
			double later = l != nullptr && re != nullptr ? bar_ink_edge(re, l->geo, true) : 0;
			if (pre != nullptr) {
				cairo_surface_destroy(pre);
			}
			if (re != nullptr) {
				cairo_surface_destroy(re);
			}
			bool reloaded = later >= before + widen_px;
			// a reload recompiles every plugin: in flight under load
			if (auto wait = wait_for(reloaded, {.poll_ms = 700, .budget_ms = 105000})) {
				shot("light-reloaded");
				return *wait;
			}
			check(reloaded, std::format("the shell reloaded on the config edit "
				"(the re-enabled plugin's label pushed the left section's edge "
				"from {} to {} px)", before, later));
			bool light = l != nullptr
				&& shot_pixel_is("light-reloaded", l->geo.x + 3, l->geo.y + l->geo.height / 2, shell_bg_light);
			if (auto wait = wait_for(light, {.poll_ms = 700, .budget_ms = 28000})) {
				shot("light-reloaded");
				return *wait;
			}
			check(light, "the reloaded shell kept the light mode (the bar stayed #e1e2e7)");
			click(theme_x, bar_mid_y()); // and back: light -> dark
			return after(500);
		}},
		{"theme_switchs_promises_the_other_way", [] {
			std::string cfg = read_file(shell_config);
			bool written = cfg.find("\"mode\": \"dark\"") != std::string::npos;
			bool told = server->theme.mode() == ThemeMode::dark;
			if (auto wait = wait_for(!(!(written && told)), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(written, "clicking the switch again wrote theme.mode: dark to the scratch shell.json");
			check(told, "the compositor reports theme dark again");
			shot("dark-restored-bar");
			return after(700);
		}},
		{"theme_bar_is_dark_again", [] {
			LayerSurface *l = bar_layer();
			bool dark = l != nullptr
				&& shot_pixel_is("dark-restored-bar", l->geo.x + 3, l->geo.y + l->geo.height / 2, shell_bg_dark);
			if (auto wait = wait_for(dark, {.poll_ms = 700, .budget_ms = 28000})) {
				shot("dark-restored-bar");
				return *wait;
			}
			check(dark, "switching back restored the dark bar (its background is #1a1b26)");
			click(power_x, bar_mid_y()); // and the panel with it
			return after(400);
		}},
		{"theme_shot_dark_restored_panel", [] {
			if (auto wait = wait_for(panel_layer("power") != nullptr, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("power") != nullptr, "the power panel opened in dark mode");
			shot("dark-restored-panel");
			return after(700);
		}},
		{"theme_panels_card_is_the_dark", [] {
			wlr_box card = card_box(power_x, power_panel);
			bool dark = shot_pixel_is("dark-restored-panel", card.x + 5, card.y + 5, shell_bg_dark);
			if (auto wait = wait_for(dark, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("dark-restored-panel");
				return *wait;
			}
			check(dark, "switching back restored the dark panel (its card is #1a1b26)");
			click_outside_cards();
			return after(400);
		}},
		{"theme_last_panel_closed", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the last panel closed");
			return after(100);
		}},
		{"theme_machines_real_color_scheme_read", [] {
			scheme_start = real_color_scheme();
			if (scheme_start.empty()) {
				std::println("note: the machine's color-scheme could not be read"
					" (no session bus); the untouched check is skipped");
			} else {
				std::println("note: the machine's color-scheme starts as {}",
					scheme_start);
			}
			return after(300);
		}},
		{"theme_outside_theme_set_the_way", [] {
			outside_theme_set("light");
			return after(400);
		}},
		{"theme_compositor_switched", [] {
			bool light = server->theme.mode() == ThemeMode::light;
			if (auto wait = wait_for(light, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(light, "the outside theme.set switched the compositor to light");
			shot("adopt-light-bar"); // the shell's answer, caught by the next step
			return after(700);
		}},
		{"theme_shell_adopted_it", [] {
			LayerSurface *l = bar_layer();
			bool light = l != nullptr
				&& shot_pixel_is("adopt-light-bar", l->geo.x + 3, l->geo.y + l->geo.height / 2, shell_bg_light);
			if (auto wait = wait_for(light, {.poll_ms = 700, .budget_ms = 28000})) {
				shot("adopt-light-bar");
				return *wait;
			}
			check(light, "the shell adopted the outside switch (the bar is #e1e2e7)");
			return after(400);
		}},
		{"theme_adoption_keeps_the_mode_like", [] {
			std::string cfg = read_file(shell_config);
			bool kept = cfg.find("\"mode\": \"light\"") != std::string::npos;
			if (auto wait = wait_for(kept, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(kept, "the adopted mode was kept (theme.mode: light in the scratch shell.json)");
			return after(300);
		}},
		{"theme_bar_before_the_reload_for", [] {
			shot("adopt-prereload");
			return after(700);
		}},
		{"theme_edit_the_shell_reloads_on", [] {
			std::string cfg = read_file(shell_config);
			std::size_t at = cfg.find("\"enabled\": true");
			if (at != std::string::npos) {
				cfg.replace(at, 15, "\"enabled\": false");
			}
			replace_file(shell_config, cfg);
			return after(500);
		}},
		{"theme_shell_reloads", [] {
			shot("adopt-reloaded");
			return after(700);
		}},
		{"theme_reloads_two_proofs_2", [] {
			LayerSurface *l = bar_layer();
			cairo_surface_t *pre = load_png("adopt-prereload");
			cairo_surface_t *re = load_png("adopt-reloaded");
			double before = l != nullptr && pre != nullptr ? bar_ink_edge(pre, l->geo, true) : 0;
			double later = l != nullptr && re != nullptr ? bar_ink_edge(re, l->geo, true) : 0;
			if (pre != nullptr) {
				cairo_surface_destroy(pre);
			}
			if (re != nullptr) {
				cairo_surface_destroy(re);
			}
			bool reloaded = later <= before - widen_px;
			// a reload recompiles every plugin: in flight under load
			if (auto wait = wait_for(reloaded, {.poll_ms = 700, .budget_ms = 105000})) {
				shot("adopt-reloaded");
				return *wait;
			}
			check(reloaded, std::format("the shell reloaded on the config edit "
				"(the disabled plugin's label pulled the left section's edge "
				"from {} to {} px)", before, later));
			bool light = l != nullptr
				&& shot_pixel_is("adopt-reloaded", l->geo.x + 3, l->geo.y + l->geo.height / 2, shell_bg_light);
			if (auto wait = wait_for(light, {.poll_ms = 700, .budget_ms = 28000})) {
				shot("adopt-reloaded");
				return *wait;
			}
			check(light, "the reloaded shell kept the adopted mode (the bar stayed #e1e2e7)");
			return after(300);
		}},
		{"theme_and_the_switch_itself_still", [] {
			click(theme_x, bar_mid_y()); // light -> dark, the ordinary way back
			return after(500);
		}},
		{"theme_switchs_promises_back_from_an", [] {
			std::string cfg = read_file(shell_config);
			bool written = cfg.find("\"mode\": \"dark\"") != std::string::npos;
			bool told = server->theme.mode() == ThemeMode::dark;
			if (auto wait = wait_for(!(!(written && told)), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(written, "clicking the switch wrote theme.mode: dark to the scratch shell.json");
			check(told, "the compositor reports theme dark again");
			shot("adopt-dark-bar");
			return after(700);
		}},
		{"theme_bar_is_dark_again_2", [] {
			LayerSurface *l = bar_layer();
			bool dark = l != nullptr
				&& shot_pixel_is("adopt-dark-bar", l->geo.x + 3, l->geo.y + l->geo.height / 2, shell_bg_dark);
			if (auto wait = wait_for(dark, {.poll_ms = 700, .budget_ms = 28000})) {
				shot("adopt-dark-bar");
				return *wait;
			}
			check(dark, "the switch restored the dark bar (its background is #1a1b26)");
			return after(300);
		}},
		{"theme_headless_run_never_wrote_the", [] {
			// Every settings write in this process (the compositor's own
			// included) was pointed at the scratch keyfile backend in main():
			// the headless backend is not the real session, so through every
			// switch of this run nothing may have landed even there.
			std::string store = shot_dir + "/xdg/config/glib-2.0/settings/keyfile";
			check(!std::filesystem::exists(store),
				"the headless run wrote no settings store (the preference is "
				"the real session's to set)");
			return after(300);
		}},
		{"theme_and_the_machines_own_setting", [] {
			if (scheme_start.empty()) {
				std::println("PASS (unchecked: no session bus to read it)");
				return after(100);
			}
			std::string scheme_end = real_color_scheme();
			check(scheme_end == scheme_start, std::format("the machine's "
				"color-scheme is untouched (still {})", scheme_start));
			return after(100);
		}},
	};
}

} // namespace bar
