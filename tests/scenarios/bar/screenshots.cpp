// A shot taken with a panel open, and the region picker.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

int shots_before = 0; // screenshots on disk before the chord under test

// The screenshots this run has saved: the compositor writes them under the
// scratch HOME's Pictures/Screenshots (src/apps/capture_ui.cpp).
int screenshots_taken() {
	std::error_code ec;
	int n = 0;
	for (auto it = std::filesystem::directory_iterator(
			scratch_home + "/Pictures/Screenshots", ec);
			!ec && it != std::filesystem::directory_iterator(); ++it) {
		n++;
	}
	return n;
}

} // namespace

std::vector<harness::Step> screenshots_steps() {
	return {
		{"screenshots_clock_panel_open", [] {
			// A screenshot with a panel open: the chord is the compositor's
			// whoever holds the keyboard (docs/intents/apps/capture.md).
			act_when(true, [] { click(clock_x, bar_mid_y()); });
			if (auto wait = wait_for(panel_layers() != 0, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() > 0, "a bar panel is open for the screenshot");
			shots_before = screenshots_taken();
			chord(KEY_3, std::array<uint32_t, 2>{KEY_LEFTMETA, KEY_LEFTSHIFT});
			return after(900);
		}},
		{"screenshots_shot_landed_all_the_same", [] {
			int now = screenshots_taken();
			if (auto wait = wait_for(now > shots_before, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(now > shots_before, std::format("Super+Shift+3 took a screenshot "
				"with a panel open ({} files, was {})", now, shots_before));
			shots_before = now;
			// The region picker with a panel open: the drag belongs to the
			// picker, not to the surface under the pointer.
			if (panel_layers() == 0) {
				click(clock_x, bar_mid_y()); // the panel again
			}
			return after(600);
		}},
		{"screenshots_panel_open", [] {
			if (auto wait = wait_for(panel_layers() != 0, {.poll_ms = 200, .budget_ms = 15000})) {
				if (polls() % 10 == 5) {
					click(clock_x, bar_mid_y()); // a click may have toggled it shut
				}
				return *wait;
			}
			check(panel_layers() > 0, "a bar panel is open for the region pick");
			chord(KEY_4, std::array<uint32_t, 2>{KEY_LEFTMETA, KEY_LEFTSHIFT});
			return after(500);
		}},
		{"screenshots_picker_owns_the_pointer", [] {
			check(server->capture_ui.owns_keyboard(),
				"Super+Shift+4 opened the picker with the panel open");
			move_to(clock_x - 60, bar_height() + 40);
			button(BTN_LEFT, true);
			for (int i = 1; i <= 6; i++) {
				move_to(clock_x - 60 + i * 20, bar_height() + 40 + i * 15);
			}
			button(BTN_LEFT, false);
			return after(900);
		}},
		{"screenshots_region_landed_as_a_file", [] {
			int now = screenshots_taken();
			if (auto wait = wait_for(now > shots_before, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(now > shots_before, std::format("a region pick with a panel open "
				"saved its shot ({} files, was {})", now, shots_before));
			// The same in the overview with the settings window open is owed:
			// the phase could not open that window reliably this late in the
			// run (docs/plans has the box).
			return after(100); // on to the last phase: a note's output going
		}},
	};
}

} // namespace bar
