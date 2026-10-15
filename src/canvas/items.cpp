#include <cmath>

#include "canvas/items.h"

#include "platform/output.h"
#include "core/server.h"

namespace {

// The canvas point a new item centres on: the middle of what the output
// under the cursor shows — its overview target when it is in the overview.
Point shown_centre(const Output &output) {
	if (output.overview.active()) {
		const wlr_box &box = output.overview.target().box;
		return Point{.x = box.x + box.width / 2.0, .y = box.y + box.height / 2.0};
	}
	Rect viewport = output.viewport();
	return Point{.x = viewport.cx(), .y = viewport.cy()};
}

} // namespace

Items::Items(Server &server) : server_(server) {
}

Item *Items::find(std::string_view id) {
	for (Item &item : items_) {
		if (item.id == id) {
			return &item;
		}
	}
	return nullptr;
}

Item *Items::set(std::string_view id, const ItemUpdate &update,
		std::optional<int> x, std::optional<int> y) {
	Item *item = find(id);
	bool created = item == nullptr;
	if (created && (!x.has_value() || !y.has_value())
			&& server_.output_under_cursor() == nullptr) {
		return nullptr; // a new item has nowhere to centre on
	}
	if (created) {
		items_.push_back(Item{.id = std::string{id}});
		item = &items_.back();
	}
	if (update.color.has_value()) {
		item->color = *update.color;
	}
	if (update.text.has_value()) {
		item->text = std::move(*update.text);
	}
	if (update.width.has_value()) {
		item->width = *update.width;
	}
	if (update.height.has_value()) {
		item->height = *update.height;
	}
	if (x.has_value()) {
		item->x = *x;
	}
	if (y.has_value()) {
		item->y = *y;
	}
	if (created && (!x.has_value() || !y.has_value())) {
		// Centre the missing coordinate on what the output under the
		// cursor shows.
		Point centre = shown_centre(*server_.output_under_cursor());
		if (!x.has_value()) {
			item->x = static_cast<int>(std::round(centre.x - item->width / 2.0));
		}
		if (!y.has_value()) {
			item->y = static_cast<int>(std::round(centre.y - item->height / 2.0));
		}
	}
	server_.damage_all();
	return item;
}

bool Items::remove(std::string_view id) {
	for (auto it = items_.begin(); it != items_.end(); ++it) {
		if (it->id == id) {
			items_.erase(it);
			server_.damage_all();
			return true;
		}
	}
	return false;
}

Item *Items::at(double cx, double cy) {
	for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
		if (cx >= it->x && cx < it->x + it->width
				&& cy >= it->y && cy < it->y + it->height) {
			return &*it;
		}
	}
	return nullptr;
}

void Items::place(Item &item, int x, int y) {
	if (item.x == x && item.y == y) {
		return;
	}
	item.x = x;
	item.y = y;
	server_.damage_all();
}

const std::vector<Item> &Items::all() const {
	return items_;
}
