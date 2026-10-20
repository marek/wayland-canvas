#pragma once

#include <span>
#include <xkbcommon/xkbcommon.h>

// The symbols a key press is matched by: the ones it produces, then the ones
// at its base level, so Super+Shift+3 ('#' on a US layout) matches the digit
// chord '3' as well. The spans belong to xkbcommon's state and keymap and
// hold for the event being handled.
class KeySyms {
public:
	KeySyms(std::span<const xkb_keysym_t> produced, std::span<const xkb_keysym_t> base)
		: produced_(produced), base_(base) {
	}

	std::span<const xkb_keysym_t> produced() const { return produced_; }

	// Calls `test` with each symbol until it returns true: the produced
	// symbols first, then the base ones the key does not produce as well.
	// Whether `test` returned true.
	template <typename Test>
	bool any(Test &&test) const {
		for (xkb_keysym_t sym : produced_) {
			if (test(sym)) {
				return true;
			}
		}
		for (xkb_keysym_t sym : base_) {
			if (!produces(sym) && test(sym)) {
				return true;
			}
		}
		return false;
	}

	// Calls `visit` with every symbol `any` would reach, in the same order.
	template <typename Visit>
	void each(Visit &&visit) const {
		any([&](xkb_keysym_t sym) {
			visit(sym);
			return false;
		});
	}

private:
	bool produces(xkb_keysym_t sym) const {
		for (xkb_keysym_t own : produced_) {
			if (own == sym) {
				return true;
			}
		}
		return false;
	}

	std::span<const xkb_keysym_t> produced_;
	std::span<const xkb_keysym_t> base_;
};
