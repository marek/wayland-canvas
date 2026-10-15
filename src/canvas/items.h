#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/geometry.h"

class Output;
class Server;

// One plugin-owned card on the canvas: what a sticky note is. The plugin
// owns the id and the content; the compositor places it, draws it, drags
// it and reports what the user did with it. An item never takes part in
// window management: it is not a view, so placement, tiling and desktop
// membership pass it by.
struct Item {
	std::string id;
	uint32_t color = 0; // 0xRRGGBBAA
	std::string text;
	int width = 0, height = 0; // canvas px
	int x = 0, y = 0; // canvas px, the top-left
};

// What item.set changes; a member left nullopt keeps the item's value.
struct ItemUpdate {
	std::optional<uint32_t> color;
	std::optional<std::string> text;
	std::optional<int> width;
	std::optional<int> height;
};

// Every canvas item, keyed by the plugin's id string.
class Items {
public:
	explicit Items(Server &server);

	// Creates the item or updates the one with that id. A coordinate left
	// nullopt keeps the old place and centres a new item on what the output
	// under the cursor shows (its overview target when it is in the
	// overview). Nullptr when a new item has no output to centre on.
	Item *set(std::string_view id, const ItemUpdate &update,
		std::optional<int> x, std::optional<int> y);
	bool remove(std::string_view id); // false when the id is unknown
	Item *find(std::string_view id);
	// The topmost item containing the canvas point; newest wins.
	Item *at(double cx, double cy);
	// Drag placement: moves and damages, no event — item.moved is the
	// release's job.
	void place(Item &item, int x, int y);

	// Oldest first; the back is the topmost.
	const std::vector<Item> &all() const;

private:
	Server &server_;
	std::vector<Item> items_;
};
