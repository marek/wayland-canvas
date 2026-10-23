#include "canvas/fullscreen_desktops.h"

#include <algorithm>
#include <memory>

#include "canvas/camera.h"
#include "core/server.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

// Whether `other` is a window `view` would cover: not itself, not one of
// its own dialogs, not a menu or tooltip its client places.
bool covered_by(const View &other, const View &view) {
	return &other != &view && other.shell_parent() != &view
		&& !placed_by_client(other.kind());
}

// The window's own dialogs move by (dx, dy), their saved boxes with them.
void move_dialogs(Server &server, const View &view, int dx, int dy) {
	for (View *other : server.views) {
		if (other->shell_parent() == &view) {
			other->translate(dx, dy);
		}
	}
}

} // namespace

FullscreenDesktops::FullscreenDesktops(Server &server)
	: server_(server) {
}

void FullscreenDesktops::enter(View &view, wlr_box covers) {
	// Back into fullscreen before the last exit was reconciled: it is still
	// on its own desktop.
	if (!view.mapped || find(view) != nullptr) {
		return;
	}
	if (std::ranges::none_of(server_.canvas.views_in(covers),
			[&view](const View *other) { return covered_by(*other, view); })) {
		return;
	}
	Rect frame = view.frame_rect();
	Output *showing = server_.output_showing(frame.cx(), frame.cy());
	const Output *sized = showing != nullptr ? showing : server_.output_under_cursor();
	if (sized == nullptr) {
		return;
	}
	Desktop *home = server_.canvas.desktop_of(view);
	Desktop &own = server_.canvas.desktops.create_next_to(
		home != nullptr ? home->frame() : covers, *sized);
	Away away{
		.view = view.session_id,
		.own = own.id(),
		.home = id_of(home),
		.home_camera = showing != nullptr ? showing->camera.destination()
			: camera_for_box(covers),
		.dx = own.frame().x - covers.x,
		.dy = own.frame().y - covers.y,
	};
	away_.push_back(away);
	move_dialogs(server_, view, away.dx, away.dy);
	// A window no screen shows gets its desktop without the view moving.
	if (showing != nullptr) {
		showing->goto_desktop(own);
		server_.focus.focus_view(&view);
	}
}

std::optional<wlr_box> FullscreenDesktops::area(const View &view) const {
	const Away *away = find(view);
	const Desktop *own = away != nullptr ? server_.canvas.desktops.find(away->own) : nullptr;
	if (own == nullptr) {
		return std::nullopt;
	}
	return own->frame();
}

void FullscreenDesktops::reconcile() {
	std::vector<Away> ending;
	std::erase_if(away_, [this, &ending](const Away &away) {
		View *view = view_of(away.view);
		if (view != nullptr && view->mapped && view->mode == WindowMode::fullscreen) {
			return false;
		}
		ending.push_back(away);
		return true;
	});
	for (const Away &away : ending) {
		come_back(away);
	}
}

const FullscreenDesktops::Away *FullscreenDesktops::find(const View &view) const {
	auto it = std::ranges::find(away_, view.session_id, &Away::view);
	return it != away_.end() ? &*it : nullptr;
}

View *FullscreenDesktops::view_of(uint64_t session_id) const {
	for (const std::unique_ptr<View> &view : server_.toplevels) {
		if (view->session_id == session_id) {
			return view.get();
		}
	}
	return nullptr;
}

void FullscreenDesktops::come_back(const Away &away) {
	View *view = view_of(away.view);
	Desktop *own = server_.canvas.desktops.find(away.own);
	if (view != nullptr) {
		// Out of fullscreen by a path that leaves it standing on its own
		// desktop — maximized there — it comes back by the way it went;
		// leaving fullscreen itself already put it back in its saved box.
		Rect frame = view->frame_rect();
		if (view->mapped && own != nullptr && own->contains(frame.cx(), frame.cy())) {
			view->move(view->x - away.dx, view->y - away.dy);
		}
		move_dialogs(server_, *view, -away.dx, -away.dy);
	}
	if (own == nullptr) {
		return; // abandoned or combined away meanwhile
	}
	std::vector<Output *> looking;
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		Rect seen = output->camera.destination().viewport(output->width, output->height);
		if (!output->overview.active()
				&& server_.canvas.desktops.best_overlap(seen) == own) {
			looking.push_back(output.get());
		}
	}
	server_.canvas.remove_desktop(away.own);
	Desktop *home = server_.canvas.desktops.find(away.home);
	for (Output *output : looking) {
		if (home != nullptr) {
			output->goto_desktop(*home);
		} else {
			output->camera.animate_to(away.home_camera);
		}
	}
}
