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
	// The views in `area` as a grid in the output's usable part.
	void tile(wlr_box area, const Output &output);
	// The views in `from_area` move into `into`, keeping their offsets; `from` is removed.
	void combine(DesktopId into, DesktopId from, wlr_box from_area);
	// The desktop frame at the point, or an output-sized box centred on it.
	wlr_box spot_at(double cx, double cy, const Output &output);

	Desktops desktops;

private:
	Server &server_;
};
