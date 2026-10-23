#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "core/util.h"

class Output;
class Server;

// The monitors: the one owner of the Output objects, in the order they came
// up, and the questions asked across them. Outputs come and go with the
// backend and the output layout. Telling every holder of a pointer to one
// that it is going is Server::output_gone's.
class Outputs {
public:
	explicit Outputs(Server &server);
	~Outputs();
	Outputs(const Outputs &) = delete;
	Outputs &operator=(const Outputs &) = delete;

	// Starts taking the backend's outputs in; needs the backend and the
	// output layout.
	void init();
	// Stops listening, before the backend and the layout go.
	void finish();

	auto begin() const { return list_.begin(); }
	auto end() const { return list_.end(); }
	const std::unique_ptr<Output> &front() const { return list_.front(); }
	const std::unique_ptr<Output> &back() const { return list_.back(); }
	std::size_t size() const { return list_.size(); }
	bool empty() const { return list_.empty(); }

	// An output's wlroots object is being destroyed: drops it. The holders
	// of a pointer to it were told first.
	void erase(const Output &output);

	void damage_all();
	// The output whose layout rectangle holds the point.
	Output *at(double lx, double ly) const;
	// The output the pointer is on, else the first one: nullptr only with none.
	Output *under_cursor() const;
	// The output whose viewport holds the canvas point, nullptr when none
	// does; `normal_view` leaves out the outputs showing the overview.
	enum class Showing { viewport, normal_view };
	Output *showing(double cx, double cy, Showing which = Showing::viewport) const;

	// Which outputs show which window may have changed; the next
	// update_visibility() works them out again. View::visibility_changed and
	// Output::visibility_changed pair this with the repaint.
	void visibility_changed() { visibility_dirty_ = true; }
	// Works every window's outputs out again when visibility_changed() said
	// they may differ.
	void update_visibility();

private:
	void handle_new_output(wlr_output *wlr);
	void handle_layout_change();
	// The lowest bit no output holds, the one it marks View::output_mask
	// with; 0 when all 32 are taken.
	uint32_t free_bit() const;

	Server &server_;
	std::vector<std::unique_ptr<Output>> list_;
	Listener new_output_, layout_change_;
	bool visibility_dirty_ = false; // see visibility_changed()
};
