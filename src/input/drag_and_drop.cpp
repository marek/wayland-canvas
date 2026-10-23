#include "input/drag_and_drop.h"

#include "core/server.h"

DragAndDrop::DragAndDrop(Server &server) : server_(server) {}

void DragAndDrop::init() {
	request_start_drag_.connect<&DragAndDrop::handle_request_start_drag>(
		server_.seat->events.request_start_drag, this);
	start_drag_.connect<&DragAndDrop::handle_start_drag>(
		server_.seat->events.start_drag, this);
}

void DragAndDrop::finish() {
	request_start_drag_.disconnect();
	start_drag_.disconnect();
	forget_icon();
}

std::optional<DragAndDrop::Icon> DragAndDrop::icon() const {
	if (icon_ == nullptr || icon_->surface == nullptr) {
		return std::nullopt;
	}
	return Icon{.surface = icon_->surface, .dx = dx_, .dy = dy_};
}

void DragAndDrop::handle_request_start_drag(
		wlr_seat_request_start_drag_event *event) {
	if (!wlr_seat_validate_pointer_grab_serial(server_.seat, event->origin,
			event->serial)) {
		if (event->drag->source != nullptr) {
			wlr_data_source_destroy(event->drag->source);
		}
		return;
	}
	// Start the drag itself; re-requesting it would re-emit this event forever.
	wlr_seat_start_pointer_drag(server_.seat, event->drag, event->serial);
}

void DragAndDrop::handle_start_drag(wlr_drag *drag) {
	dx_ = 0;
	dy_ = 0;
	icon_ = drag->icon;
	if (drag->icon == nullptr) {
		return;
	}
	icon_commit_.connect<&DragAndDrop::handle_icon_commit>(
		drag->icon->surface->events.commit, this);
	icon_destroy_.connect<&DragAndDrop::handle_icon_destroy>(
		drag->icon->events.destroy, this);
	server_.outputs.damage_all();
}

void DragAndDrop::handle_icon_commit(wlr_surface *surface) {
	dx_ += surface->current.dx;
	dy_ += surface->current.dy;
	server_.outputs.damage_all();
}

void DragAndDrop::handle_icon_destroy() {
	forget_icon();
	server_.outputs.damage_all();
}

void DragAndDrop::forget_icon() {
	icon_commit_.disconnect();
	icon_destroy_.disconnect();
	icon_ = nullptr;
	dx_ = 0;
	dy_ = 0;
}
