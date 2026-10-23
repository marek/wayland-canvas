// The scenario's state and the small helpers every phase uses.
#include "scenarios/bar/world.h"

namespace bar {

std::string shot_dir;

std::string user_plugin; // the canvas.hello plugin the plugins phase drops in

std::string staged_plugin; // where its files wait until the drop-in

pid_t bar_pid = 0; // the shell client, for the idle spawn watch

double theme_x = 0; // the theme switch's centre: the right-most bar run

double logo_x = 0; // the logo's centre: the left-most bar run

std::size_t bar_run_count = 0; // the runs on the bar once bring-up has it drawn

double power_x = 0; // the power indicator's centre: the theme phase's panel

std::string shell_config; // the scratch shell.json the theme switch writes

NoteEntry note; // the note under test

View *app_bar_target = nullptr;

wlr_box app_bar_entry{}; // its entry's square, for the flyout's rows

std::vector<pid_t> msg_children; // `wayland-canvas msg` runs, swept when done

pid_t usage_standin_pid = -1;

double agents_x = 0;  // the widget's run centre, once it shows

double bt_x = 0;      // the bluetooth indicator's run centre, to find it

int agents_runs_before = 0;     // the collector's runs before it did

std::string scratch_home;

std::string canvas_conf; // the scratch canvas.conf the compositor owns

int bar_surfaces_made = 0;

int bar_surfaces_remembered = 0;

wl_listener bar_surface_counter{};

double settings_gear_y = 0; // the gear button's band centre, on its edge

int settings_card_x = 0;

harness::Result end_run(std::string_view why, std::source_location where) {
	check(false, std::format("the run ends here: {}", why), where);
	return finish();
}

void wheel_over_bar(double x, int units, int events) {
	move_to(x, bar_mid_y());
	for (int i = 0; i < events; i++) {
		wheel(-units);
	}
}

void click_outside_cards(double x) {
	click(x, screen_h - 20.0);
}

double indicator_x(const char *name) {
	for (const Indicator &ind : indicators) {
		if (std::strcmp(ind.name, name) == 0) {
			return ind.x;
		}
	}
	return 0;
}

int backlight_percent() {
	return std::atoi(read_file(shot_dir + "/brightness-state").c_str());
}

std::string read_storage(const char *name) {
	std::string path = std::format(
		"{}/xdg/state/wayland-canvas/plugins/canvas.scratch/{}", shot_dir, name);
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return {};
	}
	std::string out;
	char buf[512];
	std::size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	return out;
}

} // namespace bar
