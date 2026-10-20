// Unit tests for src/platform/night_light_ramp.cpp: the gamma ramp a colour
// temperature makes. Run via `meson test` (test 'night-light-ramp').

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <source_location>
#include <string>
#include <vector>

#include "core/settings.h"
#include "platform/night_light_ramp.h"

namespace {

int failures = 0;

void check(bool condition, const std::string &what,
	std::source_location location = std::source_location::current()) {
	if (!condition) {
		++failures;
		std::printf("FAIL %s:%u: %s\n", location.file_name(),
			static_cast<unsigned>(location.line()), what.c_str());
	}
}

constexpr std::size_t ramp = 256;

uint16_t at(const std::vector<uint16_t> &table, std::size_t channel, std::size_t i,
	std::size_t size = ramp) {
	return table[channel * size + i];
}

void daylight_leaves_the_screen_alone() {
	std::vector<uint16_t> table = night_light_table(settings::night_light_max_k, ramp);
	check(table.size() == 3 * ramp, "three channels of ramp entries each");
	for (std::size_t channel = 0; channel < 3; channel++) {
		for (std::size_t i = 0; i < ramp; i++) {
			uint16_t identity = static_cast<uint16_t>(
				std::lround(static_cast<double>(i) / (ramp - 1) * UINT16_MAX));
			check(at(table, channel, i) == identity,
				std::format("6500 K is the identity: channel {} entry {} is {}, not {}",
					channel, i, at(table, channel, i), identity));
		}
	}
}

void the_endpoints_are_right() {
	for (int kelvin : {settings::night_light_min_k, 2000, settings::default_night_light_k, 5500}) {
		std::vector<uint16_t> table = night_light_table(kelvin, ramp);
		float multiplier[3];
		night_light_multipliers(kelvin, multiplier);
		for (std::size_t channel = 0; channel < 3; channel++) {
			check(at(table, channel, 0) == 0,
				std::format("{} K: channel {} starts at black", kelvin, channel));
			uint16_t top = static_cast<uint16_t>(
				std::lround(UINT16_MAX * static_cast<double>(multiplier[channel])));
			check(at(table, channel, ramp - 1) == top,
				std::format("{} K: channel {} ends at {}, not {}", kelvin, channel,
					at(table, channel, ramp - 1), top));
		}
	}
}

void the_channels_never_fall() {
	for (int kelvin = settings::night_light_min_k; kelvin <= settings::night_light_max_k;
			kelvin += 250) {
		std::vector<uint16_t> table = night_light_table(kelvin, ramp);
		for (std::size_t channel = 0; channel < 3; channel++) {
			for (std::size_t i = 1; i < ramp; i++) {
				check(at(table, channel, i) >= at(table, channel, i - 1),
					std::format("{} K: channel {} falls at entry {}", kelvin, channel, i));
			}
		}
	}
}

void blue_is_lowest_when_warm() {
	for (int kelvin = settings::night_light_min_k; kelvin < settings::night_light_max_k;
			kelvin += 250) {
		std::vector<uint16_t> table = night_light_table(kelvin, ramp);
		uint16_t red = at(table, 0, ramp - 1);
		uint16_t green = at(table, 1, ramp - 1);
		uint16_t blue = at(table, 2, ramp - 1);
		check(blue <= green && green <= red,
			std::format("{} K: red {} green {} blue {} (blue lowest, red highest)",
				kelvin, red, green, blue));
	}
	std::vector<uint16_t> warm = night_light_table(settings::default_night_light_k, ramp);
	check(at(warm, 2, ramp - 1) < at(warm, 0, ramp - 1),
		"4000 K takes blue strictly below red");
}

void warmer_takes_more_blue_away() {
	std::vector<uint16_t> warmer = night_light_table(2000, ramp);
	std::vector<uint16_t> milder = night_light_table(4000, ramp);
	check(at(warmer, 2, ramp - 1) < at(milder, 2, ramp - 1), "2000 K has less blue than 4000 K");
}

void any_ramp_size() {
	for (std::size_t size : {std::size_t{1}, std::size_t{2}, std::size_t{17}, std::size_t{1024}}) {
		std::vector<uint16_t> table = night_light_table(4000, size);
		check(table.size() == 3 * size, std::format("a ramp of {} entries", size));
		check(at(table, 0, size - 1, size) > 0, std::format("size {}: red is lit", size));
	}
	check(night_light_table(4000, 0).empty(), "a ramp of no entries is empty");
}

void out_of_range_temperatures_clamp() {
	check(night_light_table(10, ramp) == night_light_table(settings::night_light_min_k, ramp),
		"below the minimum is the minimum");
	check(night_light_table(20000, ramp) == night_light_table(settings::night_light_max_k, ramp),
		"above the maximum is the maximum");
}

} // namespace

int main() {
	daylight_leaves_the_screen_alone();
	the_endpoints_are_right();
	the_channels_never_fall();
	blue_is_lowest_when_warm();
	warmer_takes_more_blue_away();
	any_ramp_size();
	out_of_range_temperatures_clamp();

	if (failures == 0) {
		std::printf("ALL PASSED\n");
		return 0;
	}
	std::printf("%d failure(s)\n", failures);
	return 1;
}
