#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <vector>
#include "core/geometry.h"
#include "core/util.h"

class Server;

// A desktop's unique identity; handed out once and never reused.
enum class DesktopId : int {};

// A screen-sized rectangle on the canvas. Windows belong to the desktop their centre
// lies in; moving the frame never moves windows.
class Desktop {
public:
	Desktop(DesktopId id, int number, wlr_box frame);

	DesktopId id() const;
	int number() const; // "Desktop 2": the smallest unused number at creation
	const wlr_box &frame() const; // canvas coordinates
	Rect rect() const;
	Point centre() const;
	double scale() const; // the desktop's display scale; its frame fills the screen at it
	bool tiling() const; // the switch: while on, the desktop keeps its windows in a grid
	bool contains(double cx, double cy) const;

private:
	friend class Desktops; // the frame changes only through Desktops
	// Sets the scale and resizes the frame around its centre: base size /
	// scale. Windows never move. Callers tell the desktop's windows only
	// when the change settles.
	void set_scale(double scale);
	DesktopId id_;
	int number_;
	double scale_ = 1.0;
	int base_w_ = 0, base_h_ = 0; // the output's logical size when the desktop was made
	wlr_box frame_;
	bool tiling_ = false; // off on a new desktop
};

// Every desktop on the canvas, topmost first; the only owner of Desktop objects.
class Desktops {
public:
	explicit Desktops(Server &server);

	// The smallest unused number, placed topmost; damages.
	Desktop &create(wlr_box frame);
	void remove(DesktopId id); // damages
	void raise(DesktopId id);
	void move(DesktopId id, int x, int y); // the frame; a tiled desktop carries its windows
	void set_scale(DesktopId id, double scale); // resizes the frame around its centre; damages
	// Flips the switch only; the grid that follows is Canvas::toggle_tiling's.
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
	Server &server_;
	std::vector<std::unique_ptr<Desktop>> stack_; // front is topmost
	DesktopId next_id_ = DesktopId{1}; // handed out once, never reused
};
