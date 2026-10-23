#pragma once

#include "core/wlr.h"

class Output;

// The session lock as the input paths see it: while the session is locked
// only the locker receives input (docs/intents/shell/lock.md). The lock
// implements it and Input holds it, so no input code names the lock.
class InputGate {
public:
	InputGate() = default;
	virtual ~InputGate() = default;
	InputGate(const InputGate &) = delete;
	InputGate &operator=(const InputGate &) = delete;

	virtual bool locked() const = 0;
	// The locker's mapped surface for `output`, nullptr when it has none.
	virtual wlr_surface *surface_for(const Output &output) const = 0;
	// Input of any kind arrived: the idle countdown starts again.
	virtual void notice_activity() = 0;
};
