#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The red, green and blue multipliers of a colour temperature in kelvin:
// 6500 K is daylight and leaves the screen alone, lower is warmer.
void night_light_multipliers(int kelvin, float out[3]);

// A gamma ramp of `size` entries per channel that warms the screen to
// `kelvin`: the red, green and blue ramps one after the other, each the
// identity ramp scaled by its channel's multiplier.
std::vector<uint16_t> night_light_table(int kelvin, std::size_t size);
