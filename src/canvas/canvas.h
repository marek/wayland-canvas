#pragma once

#include <optional>
#include <vector>

#include "canvas/desktop.h"
#include "canvas/fullscreen_desktops.h"
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
	// The views in `area` whose kind tiles, as slices on a one-shot board,
	// once: a free spot has no tiling switch. They keep no board membership.
	void tile(wlr_box area, const Output &output);
	// Flips a desktop's tiling switch: on lays its standing windows onto the
	// board by the new-window rule; off leaves them where the board left
	// them, told they are no longer tiled.
	void toggle_tiling(DesktopId id);
	// Super+Left/Right/Up/Down: steps the focused view's slice and pushes the
	// windows it covers. A step that leaves a covered window nowhere to go is
	// not taken.
	void step(View &view, int axis, int dir);
	// A view's board membership may have changed (opened, closed, a state
	// toggled, a move ended): it returns to its slice when it is still free,
	// joins the board where it stands, or leaves the board behind.
	void settle(View &view);
	// Drops the view from its board: told it is no longer tiled, slice and
	// membership dropped with it.
	void leave_board(View &view);
	// A tiled view's drag ended: inside its desktop it swaps slices with the
	// window under the drop or takes the largest free slice under it; outside
	// the desktop it leaves the board.
	void drop(View &view);
	// Takes a desktop away; its windows stay where they stand. A tiled
	// desktop's are told they are no longer tiled and drop their slices.
	void remove_desktop(DesktopId id);
	// Moves a desktop's frame. A tiled desktop carries its windows with it; the
	// windows of any other stay on the canvas and may belong to another
	// desktop after.
	void move_desktop(DesktopId id, int x, int y);
	// Abandons the desktop (remove_desktop), or says there is none; a hint
	// names it.
	void abandon(std::optional<DesktopId> desktop);
	// The views in `from_area` move into `into`, keeping their offsets; `from` is removed.
	void combine(DesktopId into, DesktopId from, wlr_box from_area);
	// Combines the desktop nearest `into` into it (combine), or says why
	// nothing is combined.
	void combine_nearest(std::optional<DesktopId> into);
	// Settles a desktop's scale: the frame resizes around its centre, an
	// overview targeting it follows the frame, and a hint names the scale.
	// Nothing else follows: no window is resized or re-flowed, and no
	// client is told.
	void settle_scale(DesktopId id, double scale);
	// The desktop frame at the point, or an output-sized box centred on it.
	wlr_box spot_at(double cx, double cy, const Output &output);

	// The frames: asked freely, and created, raised and scaled in place. A
	// change that reaches a window is one of the verbs above.
	Desktops desktops;
	FullscreenDesktops fullscreen_desktops;

private:
	// The views the desktop's board holds: mapped, unminimized, and held by
	// membership through zooms and states. Maximize and fullscreen keep their
	// slice reserved while they last.
	std::vector<View *> board_views(const Desktop &desktop);
	// The slices those views hold, aligned with board_views().
	std::vector<Slice> held_slices(const Desktop &desktop);
	// The desktop's frame inset by the bars of the output that shows it,
	// at the desktop's scale.
	Rect tiling_area(const Desktop &desktop);
	// Puts the view on its slice's box, told it is tiled. Maximized and
	// fullscreen views only get the slice.
	void place_on_slice(View &view, const Desktop &desktop, Slice slice);
	// A new window on the desktop's board: the largest free slice, or half of
	// the largest slice when none is free. The view joins the board, unless
	// its kind does not tile (windows/view_kind.h).
	void join(Desktop &desktop, View &view);

	Server &server_;
};
