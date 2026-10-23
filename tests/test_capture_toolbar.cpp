// Unit tests for src/apps/capture_toolbar_layout.cpp: the capture toolbar's
// boxes and its hit-test. Self-contained: no framework. Run via `meson test`
// (test 'capture-toolbar').

#include "apps/capture_toolbar_layout.h"
#include "check.h"
#include "core/config.h"

namespace {

using unit::check;

bool same(const wlr_box &a, const wlr_box &b) {
	return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool inside(const wlr_box &inner, const wlr_box &outer) {
	return inner.x >= outer.x && inner.y >= outer.y
		&& inner.x + inner.width <= outer.x + outer.width
		&& inner.y + inner.height <= outer.y + outer.height;
}

bool apart(const wlr_box &a, const wlr_box &b) {
	return a.x + a.width <= b.x || b.x + b.width <= a.x
		|| a.y + a.height <= b.y || b.y + b.height <= a.y;
}

double centre_x(const wlr_box &box) {
	return box.x + box.width / 2.0;
}

double centre_y(const wlr_box &box) {
	return box.y + box.height / 2.0;
}

void test_wide_output() {
	const int width = 1280, height = 720;
	ToolbarLayout layout = toolbar_layout(width, height);

	// The panel is centred, a margin above the bottom.
	check(2 * layout.box.x + layout.box.width == width);
	check(layout.box.y + layout.box.height == height - config::capture_toolbar_margin);

	// Wide enough: every mode button has its full width.
	for (const wlr_box &mode : layout.modes) {
		check(mode.width == config::capture_toolbar_button_w);
		check(mode.height == config::capture_toolbar_button_h);
		check(inside(mode, layout.box));
	}
	for (const wlr_box &option : layout.options) {
		check(inside(option, layout.box));
		check(option.height == config::capture_toolbar_options_h);
	}
	check(inside(layout.records, layout.box));

	// Nothing overlaps, and the options sit above the modes.
	for (std::size_t i = 0; i < layout.modes.size(); i++) {
		for (std::size_t j = i + 1; j < layout.modes.size(); j++) {
			check(apart(layout.modes[i], layout.modes[j]));
		}
		for (const wlr_box &option : layout.options) {
			check(apart(layout.modes[i], option));
			check(option.y + option.height <= layout.modes[i].y);
		}
	}
	for (std::size_t i = 0; i + 1 < layout.options.size(); i++) {
		check(apart(layout.options[i], layout.options[i + 1]));
	}

	// The shots, a group gap on top of the button gap, the recordings.
	check(layout.modes[1].x - (layout.modes[0].x + layout.modes[0].width)
		== config::capture_toolbar_gap);
	check(layout.modes[3].x - (layout.modes[2].x + layout.modes[2].width)
		== config::capture_toolbar_gap + config::capture_toolbar_group_gap);
	check(layout.records.x == layout.modes[3].x);
	check(layout.records.x + layout.records.width == layout.modes[5].x + layout.modes[5].width);

	// Buttons are found by what they are.
	for (std::size_t i = 0; i < toolbar_modes.size(); i++) {
		check(same(layout.mode_box(toolbar_modes[i]), layout.modes[i]));
	}
	for (std::size_t i = 0; i < toolbar_options.size(); i++) {
		check(same(layout.option_box(toolbar_options[i]), layout.options[i]));
	}
	check(same(layout.mode_box({ToolbarTarget::window, true}), layout.modes[4]));
	check(same(layout.mode_box({ToolbarTarget::region, false}), layout.modes[2]));
	check(same(layout.option_box(ToolbarOption::timer), layout.options[1]));
}

void test_narrow_output() {
	// The buttons narrow with the output: here 82 px against 84.
	ToolbarLayout layout = toolbar_layout(640, 480);
	check(layout.modes[0].width == 82);
	check(2 * layout.box.x + layout.box.width == 640);
	check(layout.box.x >= 0);
}

void test_tiny_output() {
	// Tiny outputs clip, not crush: half the full width is the floor.
	ToolbarLayout layout = toolbar_layout(200, 200);
	for (const wlr_box &mode : layout.modes) {
		check(mode.width == config::capture_toolbar_button_w / 2);
	}
	check(layout.box.width > 200);
}

void test_hit() {
	ToolbarLayout layout = toolbar_layout(1280, 720);

	for (std::size_t i = 0; i < toolbar_modes.size(); i++) {
		const wlr_box &box = layout.modes[i];
		check(toolbar_hit(layout, centre_x(box), centre_y(box)) == ToolbarButton{toolbar_modes[i]});
		// The box is half-open: its last pixel hits, the next one is the gap.
		check(toolbar_hit(layout, box.x + box.width - 1, centre_y(box))
			== ToolbarButton{toolbar_modes[i]});
		check(!toolbar_hit(layout, box.x + box.width, centre_y(box)).has_value());
	}
	for (std::size_t i = 0; i < toolbar_options.size(); i++) {
		const wlr_box &box = layout.options[i];
		check(toolbar_hit(layout, centre_x(box), centre_y(box)) == ToolbarButton{toolbar_options[i]});
	}

	// The footer line, the panel's padding and everything outside hit nothing.
	check(!toolbar_hit(layout, centre_x(layout.box),
		layout.box.y + layout.box.height - 2).has_value());
	check(!toolbar_hit(layout, layout.box.x + 1, layout.box.y + 1).has_value());
	check(!toolbar_hit(layout, 0, 0).has_value());
	check(!toolbar_hit(layout, centre_x(layout.box), 0).has_value());
}

} // namespace

int main() {
	test_wide_output();
	test_narrow_output();
	test_tiny_output();
	test_hit();
	return unit::summary();
}
