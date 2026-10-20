#include "apps/capture_toolbar_layout.h"

#include "core/config.h"

ToolbarLayout toolbar_layout(int width, int height) {
	int fixed = 2 * config::capture_toolbar_pad + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	int avail = width - 2 * config::capture_thumbnail_margin - fixed;
	int bw = (avail - 5 * config::capture_toolbar_gap) / 6;
	if (bw > config::capture_toolbar_button_w) {
		bw = config::capture_toolbar_button_w;
	}
	if (bw < config::capture_toolbar_button_w / 2) {
		bw = config::capture_toolbar_button_w / 2; // tiny outputs clip, not crush
	}
	int inner = 6 * bw + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	ToolbarLayout out;
	out.box = {
		.x = (width - (inner + 2 * config::capture_toolbar_pad)) / 2,
		.y = height - config::capture_toolbar_margin
			- (2 * config::capture_toolbar_pad + config::capture_toolbar_options_h
				+ config::capture_toolbar_gap + config::capture_toolbar_button_h
				+ config::capture_toolbar_gap + config::capture_toolbar_footer_h),
		.width = inner + 2 * config::capture_toolbar_pad,
		.height = 2 * config::capture_toolbar_pad + config::capture_toolbar_options_h
			+ config::capture_toolbar_gap + config::capture_toolbar_button_h
			+ config::capture_toolbar_gap + config::capture_toolbar_footer_h,
	};
	int modes_y = out.box.y + config::capture_toolbar_pad
		+ config::capture_toolbar_options_h + config::capture_toolbar_gap;
	for (std::size_t i = 0; i < toolbar_modes.size(); i++) {
		int x = out.box.x + config::capture_toolbar_pad
			+ static_cast<int>(i) * (bw + config::capture_toolbar_gap)
			+ (toolbar_modes[i].record ? config::capture_toolbar_group_gap : 0);
		out.modes[i] = {.x = x, .y = modes_y, .width = bw,
			.height = config::capture_toolbar_button_h};
	}
	const wlr_box &first_record = out.mode_box({ToolbarTarget::screen, true});
	const wlr_box &last_record = out.mode_box({ToolbarTarget::region, true});
	out.records = {
		.x = first_record.x,
		.y = modes_y,
		.width = last_record.x + bw - first_record.x,
		.height = config::capture_toolbar_button_h,
	};
	int options_y = out.box.y + config::capture_toolbar_pad;
	int pill = (inner - 2 * config::capture_toolbar_gap) / 3;
	for (std::size_t i = 0; i < toolbar_options.size(); i++) {
		out.options[i] = {
			.x = out.box.x + config::capture_toolbar_pad
				+ static_cast<int>(i) * (pill + config::capture_toolbar_gap),
			.y = options_y,
			.width = pill,
			.height = config::capture_toolbar_options_h,
		};
	}
	return out;
}

std::optional<ToolbarButton> toolbar_hit(const ToolbarLayout &layout, double x, double y) {
	for (std::size_t i = 0; i < toolbar_modes.size(); i++) {
		if (wlr_box_contains_point(&layout.modes[i], x, y)) {
			return toolbar_modes[i];
		}
	}
	for (std::size_t i = 0; i < toolbar_options.size(); i++) {
		if (wlr_box_contains_point(&layout.options[i], x, y)) {
			return toolbar_options[i];
		}
	}
	return std::nullopt;
}
