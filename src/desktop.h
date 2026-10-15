#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <vector>
#include "geometry.h"
#include "util.h"

class Server;

// The number users see; never reused while the desktop exists.
enum class DesktopId : int {};

// A screen-sized rectangle on the canvas. Windows belong to the desktop their centre
// lies in; moving the frame never moves windows.
class Desktop {
public:
	Desktop(DesktopId id, wlr_box frame);

	DesktopId id() const;
	int number() const; // "Desktop 2"
	const wlr_box &frame() const; // canvas coordinates
	Rect rect() const;
	Point centre() const;
	bool contains(double cx, double cy) const;

private:
	friend class Desktops; // the frame changes only through Desktops
	DesktopId id_;
	wlr_box frame_;
};

// Every desktop on the canvas, topmost first; the only owner of Desktop objects.
class Desktops {
public:
	explicit Desktops(Server &server);

	// The smallest unused number, placed topmost; damages.
	Desktop &create(wlr_box frame);
	void remove(DesktopId id); // damages
	void raise(DesktopId id);
	void move(DesktopId id, int x, int y); // the frame only; damages

	// A removed desktop resolves to nullptr.
	Desktop *find(DesktopId id);
	Desktop *find(std::optional<DesktopId> id); // nullopt finds nothing
	const Desktop *find(DesktopId id) const;
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
};
