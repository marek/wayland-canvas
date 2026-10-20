#include "core/box.h"
#include "core/server.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "windows/popup.h"
#include "windows/view.h"

namespace {

// Owner view of a popup: walk the popup parent chain to its root surface.
View *popup_owner_view(wlr_xdg_popup *popup) {
	wlr_surface *surface = popup->parent;
	if (surface == nullptr) {
		return nullptr;
	}
	return View::from_surface(surface);
}

LayerSurface *popup_owner_layer(wlr_xdg_popup *popup) {
	wlr_surface *surface = popup->parent;
	if (surface == nullptr) {
		return nullptr;
	}
	while (true) {
		wlr_xdg_popup *parent = wlr_xdg_popup_try_from_wlr_surface(surface);
		if (parent == nullptr || parent->parent == nullptr) {
			break;
		}
		surface = parent->parent;
	}
	wlr_surface *root = wlr_surface_get_root_surface(surface);
	wlr_layer_surface_v1 *layer_surface =
		wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (layer_surface == nullptr) {
		return nullptr;
	}
	return static_cast<LayerSurface *>(layer_surface->data);
}

// Constrain the popup to the area its owner is visible in; the box must be
// in the root parent surface's coordinate space. Returns false when no
// owner was found (the caller then replies with a bare configure).
bool popup_unconstrain(Popup &popup) {
	wlr_xdg_popup *xdg_popup = popup.wlr;

	View *view = popup_owner_view(xdg_popup);
	if (view != nullptr) {
		Server &server = view->server;
		// The output whose viewport shows the frame centre, else the one
		// under the cursor.
		Rect frame = view->frame_rect();
		Output *picked = server.output_showing(frame.cx(), frame.cy());
		if (picked == nullptr) {
			picked = server.output_under_cursor();
		}
		if (picked != nullptr) {
			Rect viewport = picked->viewport();
			wlr_box geo = view->shell_geometry();
			// Canvas -> toplevel surface-local: the surface origin sits at
			// (view->x - geo.x, view->y - geo.y) on the canvas.
			wlr_box box = box_covering(Rect{
				viewport.x - (view->x - geo.x),
				viewport.y - (view->y - geo.y),
				viewport.w,
				viewport.h,
			});
			wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
			return true;
		}
	}

	LayerSurface *layer = popup_owner_layer(xdg_popup);
	if (layer != nullptr) {
		// Output-local rect shifted into the layer surface's space.
		wlr_box box{
			-layer->geo.x,
			-layer->geo.y,
			layer->output->width,
			layer->output->height,
		};
		wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
		return true;
	}
	return false;
}

} // namespace

Popup::Popup(Server &server, wlr_xdg_popup *wlr)
	: wlr(wlr), server_(server) {
	commit_.connect<&Popup::handle_commit>(wlr->base->surface->events.commit, this);
	destroy_.connect<&Popup::handle_destroy>(wlr->events.destroy, this);
}

Popup::~Popup() = default;

void Popup::handle_commit() {
	if (wlr->base->initial_commit) {
		if (!popup_unconstrain(*this)) {
			wlr_xdg_surface_schedule_configure(wlr->base);
		}
	}
	// Later commits damage through server.cpp's per-surface tracker.
}

void Popup::handle_destroy() {
	destroy_owned(server_.popups, this);
}
