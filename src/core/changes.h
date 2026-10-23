#pragma once

#include <bitset>
#include <cstddef>

// A fact the control socket's subscribers hear of when it changes. The
// owner of the fact flags it with Server::changed(); the Ipc reads the flags
// once per frame and says nothing for one that was not set.
enum class Change : std::size_t {
	windows,
	desktops,
	outputs,
	overview, // an output entered or left the overview: Overview::take_changed()
	settings,
	theme,
	recording,
};

inline constexpr std::size_t change_kinds = static_cast<std::size_t>(Change::recording) + 1;

class Changes {
public:
	void set(Change what) { flags_.set(static_cast<std::size_t>(what)); }
	// Whether `what` was set since the last take; clears it.
	bool take(Change what) {
		std::size_t bit = static_cast<std::size_t>(what);
		bool was = flags_.test(bit);
		flags_.reset(bit);
		return was;
	}

private:
	std::bitset<change_kinds> flags_;
};
