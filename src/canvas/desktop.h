#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <vector>
#include "core/geometry.h"
#include "core/util.h"

class Output;

class Server;

// A desktop's unique identity; handed out once and never reused.
enum class DesktopId : int {};

// A screen-sized rectangle on the canvas. Windows belong to the desktop their centre
// lies in, or whose board holds them; moving the frame never moves windows.
class Desktop {
public:
	// `frame` fills the screen at `scale`: the base size is the frame times it.
	Desktop(DesktopId id, int number, wlr_box frame, double scale);

	DesktopId id() const;
	int number() const; // "Desktop 2": the smallest unused number at creation, N when Super+N makes it
	const wlr_box &frame() const; // canvas coordinates
	Rect rect() const;
	Point centre() const; // exact: the frame resizes around it, so zooms never drift it
	double scale() const; // the zoom its picture is shown at; its frame fills the screen at it
	bool tiling() const; // the switch: while on, the desktop keeps its windows tiled
	bool contains(double cx, double cy) const;

private:
	friend class Desktops; // the frame changes only through Desktops
	// Sets the scale and resizes the frame around its centre: base size /
	// scale. Windows never move, and nothing is told to their clients.
	void set_scale(double scale);
	DesktopId id_;
	int number_;
	double scale_ = 1.0;
	int base_w_ = 0, base_h_ = 0; // the output's logical size when the desktop was made
	wlr_box frame_;
	// Kept apart from the frame's whole pixels: re-centring a rounded frame
	// on every zoom would walk it across the canvas.
	Point centre_;
	bool tiling_ = false; // off on a new desktop
};

// Every desktop on the canvas, topmost first; the only owner of Desktop objects.
class Desktops {
public:
	explicit Desktops(Server &server);

	// The smallest unused number, placed topmost, filling the screen with
	// `frame` at `scale` (a view pinned at its zoom); damages.
	Desktop &create(wlr_box frame, double scale = 1.0);
	// Super+N for a missing desktop: makes it numbered `number` — the one
	// exception to the smallest-unused rule — beside desktop 1, in slot
	// `number` - 1 of the fixed clockwise order from the top (sides first,
	// then corners), passing taken slots over and ringing outwards, never
	// overlapping another desktop; the frame fills `output`'s screen.
	Desktop &create_beside(int number, const Output &output);
	void remove(DesktopId id); // damages
	void raise(DesktopId id);
	void move(DesktopId id, int x, int y); // the frame; a tiled desktop carries its windows
	void set_scale(DesktopId id, double scale); // resizes the frame around its centre; damages
	// Flips the switch only; the board that follows is Canvas::toggle_tiling's.
	void set_tiling(DesktopId id, bool on);

	// A removed desktop resolves to nullptr.
	Desktop *find(DesktopId id);
	Desktop *find(std::optional<DesktopId> id); // nullopt finds nothing
	const Desktop *find(DesktopId id) const;
	Desktop *by_number(int number); // the desktop users call "Desktop N"
	Desktop *at(double cx, double cy); // topmost containing the point
	// Covers at least half of `area`; the most overlap wins.
	Desktop *best_overlap(const Rect &area);
	Desktop *nearest(DesktopId from); // nearest centre other than `from`
	// nav_pick_direction over the frames (hop cone), skipping `exclude`.
	Desktop *in_direction(double ox, double oy, double dx, double dy,
		std::optional<DesktopId> exclude);

	std::size_t size() const;
	bool empty() const;
	// Topmost first.
	auto all() const {
		return stack_ | std::views::transform(
			[](const std::unique_ptr<Desktop> &desktop) -> const Desktop & {
				return *desktop;
			});
	}

private:
	// Where desktop 1 sits — its frame — or, with it abandoned, the first
	// output's initial view, the box desktop 1 is made at on start-up.
	wlr_box anchor_frame() const;
	// Whether `box` overlaps any desktop's frame: a slot it covers is taken.
	bool overlaps_any(wlr_box box) const;
	// Places the new desktop topmost, filling `frame` at `scale`; damages.
	Desktop &insert(int number, wlr_box frame, double scale);
	Server &server_;
	std::vector<std::unique_ptr<Desktop>> stack_; // front is topmost
	DesktopId next_id_ = DesktopId{1}; // handed out once, never reused
};
