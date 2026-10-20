#include "platform/night_light_ramp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include "core/settings.h"

namespace {

// A black body's colour at `temp` hundred kelvin, Tanner Helland's
// approximation, each channel 0 to 1.
std::array<double, 3> black_body(double temp) {
	double red = temp <= 66 ? 255.0
		: 329.698727446 * std::pow(temp - 60, -0.1332047592);
	double green = temp <= 66
		? 99.4708025861 * std::log(temp) - 161.1195681661
		: 288.1221695283 * std::pow(temp - 60, -0.0755148492);
	double blue = temp >= 66 ? 255.0
		: (temp <= 19 ? 0.0
			: 138.5177312231 * std::log(temp - 10) - 305.0447927307);
	return {std::clamp(red / 255.0, 0.0, 1.0), std::clamp(green / 255.0, 0.0, 1.0),
		std::clamp(blue / 255.0, 0.0, 1.0)};
}

} // namespace

void night_light_multipliers(int kelvin, float out[3]) {
	// the approximation is not white at 6500 K, so each channel is taken
	// relative to its value there: daylight changes nothing
	static const std::array<double, 3> daylight =
		black_body(settings::night_light_max_k / 100.0);
	std::array<double, 3> warm = black_body(std::clamp(kelvin, settings::night_light_min_k,
		settings::night_light_max_k) / 100.0);
	for (int i = 0; i < 3; i++) {
		out[i] = static_cast<float>(std::clamp(warm[i] / daylight[i], 0.0, 1.0));
	}
}

std::vector<uint16_t> night_light_table(int kelvin, std::size_t size) {
	float multiplier[3];
	night_light_multipliers(kelvin, multiplier);
	std::vector<uint16_t> table(3 * size);
	for (std::size_t color = 0; color < 3; color++) {
		for (std::size_t i = 0; i < size; i++) {
			double input = size > 1 ? static_cast<double>(i) / static_cast<double>(size - 1) : 1.0;
			table[color * size + i] = static_cast<uint16_t>(
				std::lround(input * UINT16_MAX * multiplier[color]));
		}
	}
	return table;
}
