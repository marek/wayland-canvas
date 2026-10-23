// The wheel over the bar's volume and brightness widgets.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The stand-in sink's volume now, or -1 when it cannot be read.
double sink_volume() {
	int id = 0;
	double volume = -1;
	return pw_sink(id, volume) ? volume : -1;
}

int backlight_sets() {
	std::string log = standin_log();
	int sets = 0;
	for (std::size_t at = 0; at < log.size();) {
		std::size_t end = std::min(log.find('\n', at), log.size());
		std::string_view line{log.data() + at, end - at};
		if (line.starts_with("brightnessctl ") && line.contains(" set ")) {
			sets++;
		}
		at = end + 1;
	}
	return sets;
}

} // namespace

std::vector<harness::Step> wheel_steps() {
	return {
		{"wheel", [] {
			int sink = 0;
			double volume = 0;
			bool read = pw_sink(sink, volume);
			check(read && std::abs(volume - standin_sink_start) < 0.005 && backlight_percent() == 50,
				std::format("the stand-in sink starts at {:.2f} and the backlight at {} %",
					volume, backlight_percent()));
			check(indicator_x("audio") != 0 && indicator_x("brightness") != 0,
				"the bar shows the audio and the brightness indicators");
			wheel_over_bar(indicator_x("audio"), 120);
			return after(600);
		}},
		{"wheel_audio_stepped_once_and_brightness", [] {
			double volume = sink_volume();
			if (auto wait = wait_for(std::abs(volume - 0.55) <= 0.005, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(std::abs(volume - 0.55) <= 0.005,
				std::format("a 120-unit wheel event over Audio moved the volume one step "
					"(0.50 to {:.3f})", volume));
			check(backlight_percent() == 50 && backlight_sets() == 0,
				"and left the backlight alone");
			wheel_over_bar(indicator_x("brightness"), 120);
			return after(600);
		}},
		{"wheel_brightness_stepped_once", [] {
			if (auto wait = wait_for(backlight_percent() == 55, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(backlight_percent() == 55 && backlight_sets() == 1,
				std::format("a 120-unit wheel event over Brightness moved the backlight one "
					"step ({} %, {} writes)", backlight_percent(), backlight_sets()));
			check(std::abs(sink_volume() - 0.55) <= 0.005, "and left the volume alone");
			wheel_over_bar(indicator_x("audio"), 15);
			return after(600);
		}},
		{"wheel_15_unit_event_moved_audio", [] {
			double volume = sink_volume();
			check(std::abs(volume - 0.55) <= 0.005,
				std::format("a 15-unit wheel event moved the volume not at all ({:.3f})", volume));
			wheel_over_bar(indicator_x("brightness"), 15);
			return after(600);
		}},
		{"wheel_nor_brightness", [] {
			check(backlight_percent() == 55 && backlight_sets() == 1,
				std::format("a 15-unit wheel event moved the backlight not at all ({} %, "
					"{} writes)", backlight_percent(), backlight_sets()));
			wheel_over_bar(indicator_x("audio"), 15, 7);
			return after(600);
		}},
		{"wheel_eight_small_events_are_one", [] {
			double volume = sink_volume();
			if (auto wait = wait_for(std::abs(volume - 0.60) <= 0.005, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(std::abs(volume - 0.60) <= 0.005,
				std::format("eight 15-unit wheel events add up to one notch for Audio "
					"(0.55 to {:.3f})", volume));
			wheel_over_bar(indicator_x("brightness"), 15, 7);
			return after(600);
		}},
		{"wheel_and_for_brightness", [] {
			if (auto wait = wait_for(backlight_percent() == 60, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(backlight_percent() == 60 && backlight_sets() == 2,
				std::format("eight 15-unit wheel events add up to one notch for Brightness "
					"({} %, {} writes)", backlight_percent(), backlight_sets()));
			return after(100);
		}},
	};
}

} // namespace bar
