#include "harness/steps.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <print>
#include <ranges>

namespace harness {

namespace {

Sequencer *active = nullptr;

int at_least_one(int delay_ms) {
	return std::max(delay_ms, 1);
}

} // namespace

Sequencer &sequencer() {
	return *active;
}

Sequencer::Sequencer(std::vector<Step> steps) : steps_(std::move(steps)) {
	active = this;
	done_ = steps_.empty();
	finished_ = steps_.empty();
}

Sequencer::~Sequencer() {
	if (active == this) {
		active = nullptr;
	}
}

std::string_view Sequencer::current() const {
	return done_ ? std::string_view{} : steps_[index_].name;
}

void Sequencer::start_visit() {
	polls_.clear();
	acted_.clear();
}

Sequencer::Tick Sequencer::tick() {
	if (done_) {
		return {0, true};
	}
	Result result = steps_[index_].run();
	switch (result.kind) {
	case Result::Kind::again:
		return {at_least_one(result.delay_ms), false};
	case Result::Kind::finish:
		done_ = finished_ = true;
		return {0, true};
	case Result::Kind::after:
		index_++;
		if (index_ == steps_.size()) {
			done_ = finished_ = true;
			return {0, true};
		}
		break;
	case Result::Kind::go_to: {
		auto target = std::ranges::find(steps_, result.target, &Step::name);
		if (target == steps_.end()) {
			error_ = std::format("step {} goes to {}, which is no step",
				steps_[index_].name, result.target);
			done_ = true;
			return {0, true};
		}
		index_ = static_cast<std::size_t>(target - steps_.begin());
		break;
	}
	}
	start_visit();
	return {at_least_one(result.delay_ms), false};
}

std::optional<Result> Sequencer::wait_for(bool ready, Wait wait, std::source_location where) {
	if (ready) {
		return std::nullopt;
	}
	int allowed = std::max(wait.budget_ms / std::max(wait.poll_ms, 1), 1);
	int &made = polls_[key_of(where)];
	if (made < allowed) {
		made++;
		last_polls_ = made;
		return again(wait.poll_ms);
	}
	if (made == allowed) {
		made++;
		std::println(stderr, "  waited {} ms at {}:{} in step {}, and gave up",
			wait.budget_ms, where.file_name(), where.line(), current());
	}
	return std::nullopt;
}

} // namespace harness
