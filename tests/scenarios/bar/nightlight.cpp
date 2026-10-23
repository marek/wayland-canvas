// The night light row of the brightness panel, and the shell's reloads around it.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The mapped foot windows; the terminal the logo opens is one of them.
std::vector<View *> foot_views() {
	std::vector<View *> found;
	for (View *v : server->views) {
		if (v->mapped && v->shell_app_id() != nullptr
				&& std::strcmp(v->shell_app_id(), "foot") == 0) {
			found.push_back(v);
		}
	}
	return found;
}

std::vector<View *> foots_known; // the foot windows before the logo's right click
double night_x = 0; // the brightness indicator's centre on the bar; 0 without one

// The foot window the logo opened: the one that is not among the known.
View *logo_terminal() {
	for (View *v : foot_views()) {
		if (std::ranges::find(foots_known, v) == foots_known.end()) {
			return v;
		}
	}
	return nullptr;
}

} // namespace

std::vector<harness::Step> nightlight_steps() {
	return {
		hello_plugin_step("nightlight_hello_plugin_installed"),
		{"nightlight_night_light_on_in_the", [] {
			run_msg("settings.set", R"({"nightLight":true})", "night-on");
			return after(500);
		}},
		{"nightlight_night_light_set", [] {
			if (auto wait = wait_for(msg_done("night-on"), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(msg_done("night-on") && msg_code("night-on") == 0,
				"the night light was switched on in the compositor");
			watch_remember_bar();
			// The reload makes every panel again, with the night light already on
			// and no settings event to follow.
			replace_file(shell_config, with_hello(true));
			return after(700);
		}},
		{"nightlight_reloaded", [] {
			if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(shell_reloaded(), "the shell reloaded with the night light already on");
			watch_remember_bar();
			foots_known = foot_views();
			click(logo_x, bar_mid_y(), BTN_RIGHT);
			return after(1500);
		}},
		{"nightlight_terminal_is_up", [] {
			if (auto wait = wait_for(logo_terminal() != nullptr, {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(logo_terminal() != nullptr, "a right click on the logo opened a terminal window");
			night_x = 0;
			for (const Indicator &ind : indicators) {
				if (std::strcmp(ind.name, "brightness") == 0) {
					night_x = ind.x;
				}
			}
			check(night_x != 0, "the bar shows the brightness widget");
			if (night_x == 0) {
				return go_to("nightlight_night_light_off_again", 100);
			}
			click(night_x, bar_mid_y());
			return after(700);
		}},
		{"nightlight_shot_brightness_night", [] {
			if (auto wait = wait_for(panel_layer("brightness") != nullptr, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("brightness") != nullptr, "the brightness widget opened its panel");
			return after(0);
		}},
		{"nightlight_night_light_row_of_a", [] {
			// The switch's track is the only accent below the backlight slider
			// when it is on; the temperature slider's own fill ends short of it.
			wlr_box card = card_box(night_x, brightness_panel);
			wlr_box track_box{card.x + brightness_panel - 56, bar_height() + 72, 42, 118};
			if (auto wait = shot_holds("brightness-night", [&](cairo_surface_t *s) {
					return colour_count(s, track_box, accent_dark) > 200;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("brightness-night");
			int track = colour_count(s, track_box, accent_dark);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(track > 200, std::format("the brightness panel, made while the night light is on, "
				"shows its switch on ({} accent pixels)", track));
			click_outside_cards(); // outside the card: the panel closes
			return after(600);
		}},
		{"nightlight_night_light_off_again", [] {
			run_msg("settings.set", R"({"nightLight":false})", "night-off");
			return after(400);
		}},
		{"nightlight_night_light_cleared", [] {
			if (auto wait = wait_for(msg_done("night-off"), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			watch_remember_bar();
			replace_file(shell_config, with_hello(false));
			return after(700);
		}},
		{"nightlight_reloaded_again", [] {
			if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(shell_reloaded(), "the shell reloaded with the logo's terminal open");
			watch_remember_bar();
			View *terminal = logo_terminal();
			check(terminal != nullptr, "the terminal the logo opened outlives the bar's rebuild");
			if (terminal != nullptr) {
				terminal->close();
			}
			return after(800);
		}},
		{"nightlight_terminal_gone", [] {
			if (auto wait = wait_for(logo_terminal() == nullptr, {.poll_ms = 250, .budget_ms = 15000})) {
				return *wait;
			}
			return after(100);
		}},
	};
}

} // namespace bar
