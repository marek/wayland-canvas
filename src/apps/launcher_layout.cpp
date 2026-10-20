#include "apps/launcher_layout.h"

#include <algorithm>

PanelLayout panel_layout(int width, int height) {
	int panel_w = std::min(config::launcher_width,
		std::max(width - 2 * config::launcher_margin_x, 1));
	int chrome = config::launcher_pad + config::launcher_search_height + config::launcher_gap
		+ config::launcher_gap + config::launcher_footer_height;
	int rows = std::clamp((height - 2 * config::launcher_margin_y - chrome)
			/ config::launcher_row_height,
		1, config::launcher_max_rows);
	int panel_h = chrome + rows * config::launcher_row_height;

	PanelLayout layout;
	layout.row_count = rows;
	layout.box = {(width - panel_w) / 2, (height - panel_h) / 2, panel_w, panel_h};
	layout.search = {layout.box.x + config::launcher_pad, layout.box.y + config::launcher_pad,
		panel_w - 2 * config::launcher_pad, config::launcher_search_height};
	layout.rows = {layout.search.x, layout.search.y + layout.search.height + config::launcher_gap,
		layout.search.width, rows * config::launcher_row_height};
	layout.footer = {layout.box.x, layout.box.y + panel_h - config::launcher_footer_height,
		panel_w, config::launcher_footer_height};
	return layout;
}

std::optional<int> PanelLayout::row_at(double x, double y) const {
	if (!wlr_box_contains_point(&rows, x, y)) {
		return std::nullopt;
	}
	return static_cast<int>((y - rows.y) / config::launcher_row_height);
}
