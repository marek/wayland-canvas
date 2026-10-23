#include "harness/check.h"

#include <cmath>
#include <cstdio>
#include <print>

namespace harness {

namespace {

int failed = 0;

void report(bool passed, std::string_view line, const std::source_location &where) {
	std::println("{}: {}", passed ? "PASS" : "FAIL", line);
	std::fflush(stdout);
	if (passed) {
		return;
	}
	failed++;
	std::println(stderr, "  at {}:{} in {}", where.file_name(), where.line(),
		where.function_name());
}

} // namespace

void check(bool condition, std::string_view note, std::source_location where) {
	report(condition, note, where);
}

void check_near(double got, double expected, double tolerance, std::string_view note,
		std::source_location where) {
	bool near = std::fabs(got - expected) <= tolerance;
	if (near) {
		report(true, note, where);
		return;
	}
	report(false, std::format("{}: {} where {} (± {}) was expected", note, got, expected,
		tolerance), where);
}

int failures() {
	return failed;
}

} // namespace harness
