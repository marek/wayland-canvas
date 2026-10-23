#include "windows/focus.h"

#include <algorithm>

#include "core/server.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "windows/view_kind.h"

std::optional<ViewHit> Focus::view_at(double cx, double cy, double zoom) const {
	for (View *view : server_.views) {
		if (!view->mapped || view->minimized) {
			continue;
		}
		wlr_box geo = view->shell_geometry();
		// Canvas point -> toplevel surface-local coordinates.
		double lx = cx - (view->x - geo.x);
		double ly = cy - (view->y - geo.y);
		double sub_x = 0, sub_y = 0;
		wlr_surface *found = view->surface_at(lx, ly, &sub_x, &sub_y);
		if (found != nullptr) {
			return ViewHit{view, found, sub_x, sub_y, DecoPart::none, 0};
		}
		DecoHit hit = view->deco.hit(cx, cy, zoom);
		if (hit.part != DecoPart::none) {
			return ViewHit{view, nullptr, 0, 0, hit.part, hit.edges};
		}
	}
	return std::nullopt;
}

View *Focus::top_visible_view(Output *output) const {
	for (View *view : server_.views) {
		if (!view->mapped || view->minimized || !takes_focus(view->kind())) {
			continue;
		}
		if (output == nullptr
				|| view->frame_rect().intersects(output->viewport())) {
			return view;
		}
	}
	return nullptr;
}

View *Focus::last_minimized_view() const {
	View *best = nullptr;
	for (View *view : server_.views) {
		if (!view->minimized) {
			continue;
		}
		if (best == nullptr || view->minimize_seq > best->minimize_seq) {
			best = view;
		}
	}
	return best;
}

void Focus::focus_view(View *view) {
	View *prev = view_;
	if (prev != view) {
		server_.changed(Change::windows);
		view_ = view;
		if (prev != nullptr) {
			prev->focus_changed(false);
		}
		if (view != nullptr) {
			// Raise to the top of the stacking order.
			std::erase(server_.views, view);
			server_.views.insert(server_.views.begin(), view);
			view->focus_changed(true);
		}
	}
	// The keyboard follows the focused window. An exclusive layer surface
	// (a lock screen) keeps it; an on-demand one (a panel) gives it up to a
	// window being focused, the one already focused behind it included.
	if (view != nullptr && layer_ != nullptr && !layer_->holds_keyboard_exclusively()) {
		layer_ = nullptr;
	}
	if (layer_ == nullptr) {
		focus_keyboard(view != nullptr ? view->surface() : nullptr);
	}
}

void Focus::focus_in(wlr_box box) {
	if (view_ != nullptr) {
		Rect frame = view_->frame_rect();
		if (wlr_box_contains_point(&box, frame.cx(), frame.cy())) {
			return;
		}
	}
	for (View *view : server_.canvas.views_in(box)) {
		if (takes_focus(view->kind())) {
			focus_view(view);
			return;
		}
	}
}

void Focus::focus_topmost() {
	focus_view(top_visible_view(server_.outputs.under_cursor()));
}

void Focus::view_minimized(View &view) {
	view.minimize_seq = ++minimize_counter_;
	if (view_ == &view) {
		focus_topmost();
	}
}

void Focus::forget(View &view) {
	if (view_ == &view) {
		view_ = nullptr;
	}
}

void Focus::forget(LayerSurface &layer) {
	if (layer_ == &layer) {
		layer_ = nullptr;
	}
}

void Focus::focus_keyboard(wlr_surface *surface) {
	if (surface == nullptr) {
		wlr_seat_keyboard_notify_clear_focus(server_.seat);
		return;
	}
	// With no keyboard yet the focus is still recorded: wlroots enters the
	// surface once its client binds the first keyboard that appears.
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(server_.seat);
	if (keyboard == nullptr) {
		wlr_seat_keyboard_notify_enter(server_.seat, surface, nullptr, 0, nullptr);
		return;
	}
	wlr_seat_keyboard_notify_enter(server_.seat, surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

void Focus::offer_keyboard(LayerSurface &layer, wlr_surface *surface) {
	if (layer_ == nullptr || layer_ == &layer || !layer_->holds_keyboard_exclusively()) {
		layer_ = &layer;
		focus_keyboard(surface);
	}
}

void Focus::return_keyboard() {
	layer_ = nullptr;
	focus_keyboard(view_ != nullptr && view_->mapped ? view_->surface() : nullptr);
}

void Focus::refocus_layers() {
	if (LayerSurface *holder = exclusive_keyboard_holder(server_)) {
		layer_ = holder;
		focus_keyboard(holder->wlr->surface);
		return;
	}
	if (layer_ == nullptr || layer_->mapped) {
		// A still-mapped on-demand surface keeps the keyboard it got on
		// map or by a click.
		return;
	}
	return_keyboard();
}
