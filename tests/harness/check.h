#pragma once

// The scenarios' assertions. Every check prints one PASS or FAIL line; a
// failure also prints where it was called from, and is counted. The run's
// exit status follows failures().

#include <source_location>
#include <string_view>

namespace harness {

void check(bool condition, std::string_view note,
	std::source_location where = std::source_location::current());

// Passes when `got` is within `tolerance` of `expected`; a failure's line
// carries both numbers.
void check_near(double got, double expected, double tolerance, std::string_view note,
	std::source_location where = std::source_location::current());

int failures();

} // namespace harness
