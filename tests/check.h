#pragma once

// The unit tests' shared assertions. A failed check prints the file and line
// it was called from and is counted; main ends with `return unit::summary();`.

#include <cmath>
#include <print>
#include <source_location>
#include <string_view>

namespace unit {

inline int failures = 0;

inline void check(bool condition, std::string_view note = {},
	std::source_location where = std::source_location::current()) {
	if (condition) {
		return;
	}
	++failures;
	std::println("FAIL {}:{}{}{}", where.file_name(), where.line(),
		note.empty() ? "" : ": ", note);
}

inline void check_near(double got, double expected, double tolerance,
	std::source_location where = std::source_location::current()) {
	if (std::fabs(got - expected) <= tolerance) {
		return;
	}
	++failures;
	std::println("FAIL {}:{}: {}, expected {} (tolerance {})", where.file_name(),
		where.line(), got, expected, tolerance);
}

// Prints the run's verdict and returns the process exit status.
inline int summary() {
	if (failures == 0) {
		std::println("ALL PASSED");
		return 0;
	}
	std::println("{} failure(s)", failures);
	return 1;
}

} // namespace unit
