#pragma once

#include <optional>
#include "core/util.h"

class Server;

// Drag and drop between clients: the seat starts the drag its source asked
// for, and the compositor draws the icon the drag brings at the cursor.
class DragAndDrop {
public:
	explicit DragAndDrop(Server &server);
	DragAndDrop(const DragAndDrop &) = delete;
	DragAndDrop &operator=(const DragAndDrop &) = delete;

	void init(); // listens to the seat's drag requests
	void finish(); // disconnects before the seat is destroyed

	// The surface of the icon the drag in progress shows, and its offset
	// from the cursor in layout px.
	struct Icon {
		wlr_surface *surface;
		double dx, dy;
	};
	std::optional<Icon> icon() const;

private:
	void handle_request_start_drag(wlr_seat_request_start_drag_event *event);
	void handle_start_drag(wlr_drag *drag);
	void handle_icon_commit(wlr_surface *surface);
	void handle_icon_destroy();
	void forget_icon();

	Server &server_;
	wlr_drag_icon *icon_ = nullptr;
	double dx_ = 0, dy_ = 0;
	Listener request_start_drag_, start_drag_;
	Listener icon_commit_, icon_destroy_;
};
