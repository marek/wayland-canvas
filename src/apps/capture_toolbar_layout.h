#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <variant>

#include "core/wlr.h"

// What a mode button captures.
enum class ToolbarTarget { screen, window, region };

// A mode button: a shot or a recording of one target.
struct ToolbarMode {
	ToolbarTarget target = ToolbarTarget::screen;
	bool record = false;
	bool operator==(const ToolbarMode &) const = default;
};

// An option pill: where a shot goes, the countdown, the cursor in shots.
enum class ToolbarOption { destination, timer, pointer };

// The mode buttons left to right: the shots, then the recordings.
inline constexpr std::array<ToolbarMode, 6> toolbar_modes{{
	{ToolbarTarget::screen, false},
	{ToolbarTarget::window, false},
	{ToolbarTarget::region, false},
	{ToolbarTarget::screen, true},
	{ToolbarTarget::window, true},
	{ToolbarTarget::region, true},
}};

// The option pills left to right.
inline constexpr std::array<ToolbarOption, 3> toolbar_options{
	ToolbarOption::destination, ToolbarOption::timer, ToolbarOption::pointer};

using ToolbarButton = std::variant<ToolbarMode, ToolbarOption>;

// The toolbar's on-screen boxes, output-local logical px, shared by
// drawing, hit-testing and the tests that click it.
struct ToolbarLayout {
	wlr_box box{};
	std::array<wlr_box, toolbar_modes.size()> modes{}; // as toolbar_modes
	std::array<wlr_box, toolbar_options.size()> options{}; // as toolbar_options
	wlr_box records{}; // the recording buttons together

	const wlr_box &mode_box(ToolbarMode mode) const {
		auto at = std::ranges::find(toolbar_modes, mode);
		return modes[static_cast<std::size_t>(at - toolbar_modes.begin())];
	}

	const wlr_box &option_box(ToolbarOption option) const {
		auto at = std::ranges::find(toolbar_options, option);
		return options[static_cast<std::size_t>(at - toolbar_options.begin())];
	}
};

// The toolbar on an output of `width` x `height` logical px: centred above
// the bottom margin, its buttons narrowing with the output down to half
// their width.
ToolbarLayout toolbar_layout(int width, int height);

// The button under the output-local point (x, y), if any.
std::optional<ToolbarButton> toolbar_hit(const ToolbarLayout &layout, double x, double y);
