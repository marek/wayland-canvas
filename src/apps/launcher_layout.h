#pragma once

#include <optional>

#include "core/config.h"
#include "core/wlr.h"

// The launcher panel's boxes on an output, output-local logical px, shared by
// drawing, hit-testing and the tests that click it.
struct PanelLayout {
	wlr_box box{}; // the card
	wlr_box search{}; // the search field
	wlr_box rows{}; // the visible result rows together
	wlr_box footer{}; // the key hints and the count
	int row_count = 0; // how many rows are visible at most

	// The `index`th visible row.
	wlr_box row(int index) const {
		return {rows.x, rows.y + index * config::launcher_row_height, rows.width,
			config::launcher_row_height};
	}

	// The visible row the output-local point (x, y) is on, if any.
	std::optional<int> row_at(double x, double y) const;
};

// The panel on an output of `width` x `height` logical px: centred, as wide
// as config::launcher_width allows and with as many rows as fit.
PanelLayout panel_layout(int width, int height);
