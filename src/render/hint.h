#pragma once

#include <optional>
#include <string>
#include <string_view>
#include "core/util.h"
#include "core/wlr.h"

class Server;
struct RenderCtx;

// Transient banner at the bottom of every output.
class Hint {
public:
	explicit Hint(Server &server);
	~Hint();
	Hint(const Hint &) = delete;
	Hint &operator=(const Hint &) = delete;

	// Shows `text` for about 2 s; empty clears. The text already showing
	// only has its 2 s restarted.
	void set(std::string_view text);
	// Restarts the 2 s of the text showing; nothing when none shows.
	void keep();
	void finish(); // releases the timer before the display goes

	// The text showing; empty when none.
	const std::string &text() const { return text_; }

	// Draws the banner centred on `ctx`'s target, bottom-aligned to `row`
	// (the overview's hint row), or a fixed gap above the output's bottom
	// edge when there is none. Nothing when no text shows.
	void draw(const RenderCtx &ctx, std::optional<wlr_box> row) const;

private:
	void clear();
	void restart_timer();

	Server &server_;
	std::string text_;
	EventSourcePtr timer_;
};
