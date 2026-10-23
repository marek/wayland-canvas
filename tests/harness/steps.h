#pragma once

// The scenarios' step driver. A scenario is a table of named steps; the
// driver runs one step per timer tick of the compositor's event loop, so the
// compositor, its clients and the scenario share one thread and a step may
// read and drive the compositor directly.
//
// A step returns what happens next:
//
//   after(ms)        the next step of the table, once ms have passed
//   again(ms)        this step once more, once ms have passed
//   go_to(name, ms)  the step called name, once ms have passed
//   finish()         the run's end; so is running past the table's last step
//
// A delay of 0 means the next tick. Moving to another step ends a visit: a
// step that comes round again starts with fresh waits.
//
// A wait is a condition a step cannot go on without. `wait_for(ready)` is
// nothing once `ready` holds; until then it is the `again` that polls, so a
// step reads
//
//     if (auto wait = wait_for(locker_count("LOCKED") > 0)) return *wait;
//
// and goes on below that line when the condition came true. Every wait has
// its own budget (`Wait::budget_ms`, spent in polls of `Wait::poll_ms`),
// counted per call site, so a second wait in a step never starts from the
// first one's count. A wait whose budget is spent is nothing as well and
// says so on stderr: the checks that follow report what did not come true.
//
// A step that interacts and then waits for the answer runs again for each
// poll, so its interaction goes through `act_when(ready, act)`: once `ready`
// holds, `act` runs, and only in the first run of the visit that gets there.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace harness {

struct Result {
	enum class Kind { after, again, go_to, finish };
	Kind kind;
	int delay_ms;
	std::string_view target; // the step a go_to names
};

constexpr Result after(int delay_ms) {
	return {Result::Kind::after, delay_ms, {}};
}
constexpr Result again(int delay_ms) {
	return {Result::Kind::again, delay_ms, {}};
}
constexpr Result go_to(std::string_view step, int delay_ms = 0) {
	return {Result::Kind::go_to, delay_ms, step};
}
constexpr Result finish() {
	return {Result::Kind::finish, 0, {}};
}

struct Step {
	std::string_view name;
	std::function<Result()> run;
};

struct Wait {
	int poll_ms = 100;
	int budget_ms = 10'000;
};

// Runs a table of steps and holds the waits of the visit under way. The
// free functions below speak to the one that exists.
class Sequencer {
public:
	explicit Sequencer(std::vector<Step> steps);
	~Sequencer();
	Sequencer(const Sequencer &) = delete;
	Sequencer &operator=(const Sequencer &) = delete;

	struct Tick {
		int delay_ms; // until the next tick
		bool done; // the run is over; delay_ms means nothing
	};
	// Runs the current step and moves to the step its result names.
	Tick tick();

	std::string_view current() const;
	// True once a step finished the run or it ran past the last step.
	bool finished() const { return finished_; }
	// A step that ends the run another way - the compositor's own quit - says
	// so first: the display stopping is then the run's end, not a stop before it.
	void end_is_coming() { finished_ = true; }
	// What stopped the run when a go_to named no step; empty otherwise.
	const std::string &error() const { return error_; }

	std::optional<Result> wait_for(bool ready, Wait wait = {},
		std::source_location where = std::source_location::current());
	// How many polls the wait that last said "again" has made in this visit,
	// counting that one: for a step that repeats an interaction every few
	// polls of its wait, such as a click that may have been lost.
	int polls() const { return last_polls_; }

	template <typename Act>
	std::optional<Result> act_when(bool ready, Act &&act, Wait wait = {},
			std::source_location where = std::source_location::current()) {
		if (acted_.contains(key_of(where))) {
			return std::nullopt;
		}
		if (std::optional<Result> more = wait_for(ready, wait, where)) {
			return more;
		}
		acted_.insert(key_of(where));
		act();
		return std::nullopt;
	}

private:
	using Key = std::tuple<std::string_view, uint32_t, uint32_t>;
	static Key key_of(const std::source_location &where) {
		return {where.file_name(), where.line(), where.column()};
	}
	void start_visit();

	std::vector<Step> steps_;
	std::size_t index_ = 0;
	bool done_ = false, finished_ = false;
	std::string error_;
	std::map<Key, int> polls_; // per wait, this visit
	int last_polls_ = 0;
	std::set<Key> acted_; // per act_when, this visit
};

// The sequencer of the run under way.
Sequencer &sequencer();

inline std::optional<Result> wait_for(bool ready, Wait wait = {},
		std::source_location where = std::source_location::current()) {
	return sequencer().wait_for(ready, wait, where);
}

inline int polls() {
	return sequencer().polls();
}

template <typename Act>
std::optional<Result> act_when(bool ready, Act &&act, Wait wait = {},
		std::source_location where = std::source_location::current()) {
	return sequencer().act_when(ready, std::forward<Act>(act), wait, where);
}

} // namespace harness
