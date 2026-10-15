#pragma once

#include <optional>
#include <vector>

#include "canvas/desktop.h"
#include "core/util.h"

class Output;
class Server;
class View;

// The infinite canvas: its desktops and the arrangements that act on its regions.
class Canvas {
public:
	explicit Canvas(Server &server);

	// Mapped, unminimized views whose centre lies in `box`, topmost first; at most 64.
	std::vector<View *> views_in(wlr_box box) const;
	// The desktop the view's centre lies in.
	Desktop *desktop_of(const View &view);
	// The views in `area` as a grid in the output's usable part, once: a
	// free spot has no tiling switch.
	void tile(wlr_box area, const Output &output);
	// Flips a desktop's tiling switch: on re-flows its windows into the
	// grid; off leaves them where the grid put them, told they are no
	// longer tiled.
	void toggle_tiling(DesktopId id);
	// Re-flows a tiled desktop's grid: the entry point for a settled scale
	// or frame change.
	void reflow(DesktopId id);
	// A view's grid membership may have changed (opened, closed, a state
	// toggled, a move ended): it joins or leaves the grid, at the slot
	// nearest where it stands, and the grids it left re-flow.
	void settle(View &view);
	// The views in `from_area` move into `into`, keeping their offsets; `from` is removed.
	void combine(DesktopId into, DesktopId from, wlr_box from_area);
	// The desktop's scale settled at a new value: its windows are told the
	// scale they are shown at, and a tiled grid re-flows into the resized
	// frame.
	void scale_settled(const Desktop &desktop);
	// The desktop frame at the point, or an output-sized box centred on it.
	wlr_box spot_at(double cx, double cy, const Output &output);

	Desktops desktops;

private:
	// The views in the desktop's grid: mapped, unminimized, in no state
	// that holds a window out of the grid, and its members by frame centre.
	std::vector<View *> grid_members(const Desktop &desktop);
	bool grid_member(const View &view, const Desktop &desktop);
	// The desktop's frame inset by the bars of the output that shows it.
	Rect tiling_area(const Desktop &desktop);

	Server &server_;
};
