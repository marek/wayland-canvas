#include "platform/layer.h"

#include <algorithm>

#include "platform/output.h"
#include "core/server.h"
#include "windows/view.h"

namespace {

// Layers arranged topmost first, so exclusive zones stack sensibly.
constexpr zwlr_layer_shell_v1_layer layer_order[] = {
	ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
	ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
	ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
};

// Position and size one layer surface; same math as
// wlr_scene_layer_surface_v1_configure. Updates `usable` when the surface
// reserves an exclusive zone.
void layer_configure(LayerSurface &layer, const wlr_box &full, wlr_box &usable) {
	wlr_layer_surface_v1 *ls = layer.wlr;
	wlr_layer_surface_v1_state *state = &ls->current;

	// Zone -1: cover the whole output, ignoring other exclusive zones.
	wlr_box bounds = state->exclusive_zone == -1 ? full : usable;

	wlr_box box = {
		.width = static_cast<int>(state->desired_width),
		.height = static_cast<int>(state->desired_height),
	};

	// Horizontal positioning.
	if (box.width == 0) {
		box.x = bounds.x + state->margin.left;
		box.width = bounds.width - (state->margin.left + state->margin.right);
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT &&
			state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x = bounds.x + bounds.width / 2 - box.width / 2;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
		box.x = bounds.x + state->margin.left;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x = bounds.x + bounds.width - box.width - state->margin.right;
	} else {
		box.x = bounds.x + bounds.width / 2 - box.width / 2;
	}

	// Vertical positioning.
	if (box.height == 0) {
		box.y = bounds.y + state->margin.top;
		box.height = bounds.height - (state->margin.top + state->margin.bottom);
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP &&
			state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y = bounds.y + bounds.height / 2 - box.height / 2;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
		box.y = bounds.y + state->margin.top;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y = bounds.y + bounds.height - box.height - state->margin.bottom;
	} else {
		box.y = bounds.y + bounds.height / 2 - box.height / 2;
	}

	layer.geo = box;
	// configure() asserts on surfaces that have not done their initial commit.
	if (ls->initialized) {
		wlr_layer_surface_v1_configure(ls, box.width, box.height);
	}

	if (ls->surface->mapped && state->exclusive_zone > 0) {
		wlr_edges edge = wlr_layer_surface_v1_get_exclusive_edge(ls);
		switch (edge) {
		case WLR_EDGE_TOP:
			usable.y += state->exclusive_zone + state->margin.top;
			usable.height -= state->exclusive_zone + state->margin.top;
			break;
		case WLR_EDGE_BOTTOM:
			usable.height -= state->exclusive_zone + state->margin.bottom;
			break;
		case WLR_EDGE_LEFT:
			usable.x += state->exclusive_zone + state->margin.left;
			usable.width -= state->exclusive_zone + state->margin.left;
			break;
		case WLR_EDGE_RIGHT:
			usable.width -= state->exclusive_zone + state->margin.right;
			break;
		default:
			return;
		}
		if (usable.width < 0) {
			usable.width = 0;
		}
		if (usable.height < 0) {
			usable.height = 0;
		}
	}
}

// Takes `layer` out of whichever of its output's vectors holds it.
std::unique_ptr<LayerSurface> detach(LayerSurface &layer) {
	for (std::vector<std::unique_ptr<LayerSurface>> &vec : layer.output.layers) {
		auto it = std::ranges::find_if(vec,
			[&layer](const std::unique_ptr<LayerSurface> &p) { return p.get() == &layer; });
		if (it != vec.end()) {
			std::unique_ptr<LayerSurface> owned = std::move(*it);
			vec.erase(it);
			return owned;
		}
	}
	return nullptr;
}

} // namespace

LayerSurface::LayerSurface(Server &server, Output &output, wlr_layer_surface_v1 *wlr)
		: server(server), output(output), wlr(wlr) {
	wlr->data = this;
	map_.connect<&LayerSurface::handle_map>(wlr->surface->events.map, this);
	unmap_.connect<&LayerSurface::handle_unmap>(wlr->surface->events.unmap, this);
	commit_.connect<&LayerSurface::handle_commit>(wlr->surface->events.commit, this);
	destroy_.connect<&LayerSurface::handle_destroy>(wlr->events.destroy, this);
}

LayerSurface::~LayerSurface() = default;

bool LayerSurface::wants_keyboard() const {
	return wlr->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}

bool LayerSurface::holds_keyboard_exclusively() const {
	return wlr->current.keyboard_interactive
		== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

// A placement the plugin asked for before this surface existed applies as
// it maps, so the ask can never race the surface.
void LayerSurface::take_pending_place() {
	auto found = server.canvas_places.find(name());
	if (found != server.canvas_places.end()) {
		canvas_box = found->second;
	}
}

void LayerSurface::handle_map() {
	take_pending_place();
	mapped = true;
	if (attached_) {
		// The surface's exclusive zone (if any) takes effect now.
		output.arrange_layers();
	}
	// A surface above the windows that takes keyboard input (a panel, a
	// launcher) gets the keyboard as it appears, as a new window does,
	// unless an exclusive surface holds it.
	zwlr_layer_shell_v1_layer layer = wlr->current.layer;
	bool above_windows = layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP
		|| layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	LayerSurface *holder = server.focused_layer;
	if (above_windows && wants_keyboard()
			&& (holder == nullptr || !holder->holds_keyboard_exclusively())) {
		server.focused_layer = this;
		server.focus_keyboard(wlr->surface);
	}
	server.refocus_layers();
}

void LayerSurface::handle_unmap() {
	mapped = false;
	if (attached_) {
		// Release the exclusive zone the surface was reserving.
		output.arrange_layers();
	}
	server.refocus_layers();
}

void LayerSurface::handle_commit() {
	wlr_layer_surface_v1 *ls = wlr;
	// current.committed holds the state fields changed by this commit.
	if (!ls->initial_commit && ls->current.committed == 0) {
		return;
	}
	if (!attached_) {
		return;
	}
	if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		// Layer change: move to the vector of the new layer, at its head.
		std::unique_ptr<LayerSurface> owned = detach(*this);
		if (owned != nullptr) {
			output.layers[ls->current.layer].insert(
				output.layers[ls->current.layer].begin(), std::move(owned));
		}
	}
	output.arrange_layers();
}

void LayerSurface::handle_destroy() {
	map_.disconnect();
	unmap_.disconnect();
	commit_.disconnect();
	destroy_.disconnect();

	// Off the output's lists before anything below looks at them.
	std::unique_ptr<LayerSurface> owned = detach(*this);

	if (server.focused_layer == this) {
		server.focused_layer = nullptr;
	}
	if (attached_) {
		output.arrange_layers();
	}
	server.refocus_layers();
} // `owned` releasing destroys this

std::string LayerSurface::name() const {
	return wlr->namespace_ != nullptr ? std::string{wlr->namespace_} : std::string{};
}

void Output::arrange_layers() {
	wlr_box full{0, 0, width, height};
	wlr_box usable = full;

	// Surfaces reserving an exclusive zone are arranged first (overlay,
	// top, bottom, background) so the rest fill what is left.
	for (int pass = 0; pass < 2; pass++) {
		for (std::size_t i = 0; i < std::size(layer_order); i++) {
			for (const std::unique_ptr<LayerSurface> &layer : layers[layer_order[i]]) {
				bool exclusive = layer->wlr->current.exclusive_zone > 0;
				if ((pass == 0) != exclusive) {
					continue;
				}
				if (layer->canvas_box.has_value()) {
					// On the plane: the client is sized by the rectangle
					// the plugin named, not by the output.
					layer->geo = *layer->canvas_box;
					wlr_layer_surface_v1_configure(layer->wlr,
						static_cast<uint32_t>(layer->canvas_box->width),
						static_cast<uint32_t>(layer->canvas_box->height));
					continue;
				}
				layer_configure(*layer, full, usable);
			}
		}
	}

	this->usable = usable;
	damage();
	server.visibility_dirty = true;
}

// A surface a plugin put on the plane, under an output-local point: the
// point is taken to the canvas first, so the hit follows the camera.
std::optional<LayerHit> Output::canvas_surface_at(double ox, double oy) const {
	Point on_plane = local_to_canvas(ox, oy);
	for (const auto &surfaces : layers) {
		for (const std::unique_ptr<LayerSurface> &l : surfaces) {
			if (!l->mapped || !l->canvas_box.has_value()) {
				continue;
			}
			const wlr_box &box = *l->canvas_box;
			double sx = on_plane.x - box.x, sy = on_plane.y - box.y;
			if (sx < 0 || sy < 0 || sx >= box.width || sy >= box.height) {
				continue;
			}
			double in_x = 0, in_y = 0;
			wlr_surface *surface = wlr_surface_surface_at(l->wlr->surface, sx, sy,
				&in_x, &in_y);
			if (surface != nullptr) {
				return LayerHit{.layer = l.get(), .surface = surface,
					.sx = in_x, .sy = in_y};
			}
		}
	}
	return std::nullopt;
}

std::optional<LayerHit> Output::layer_surface_at(int layer, double ox, double oy) const {
	for (const std::unique_ptr<LayerSurface> &l : layers[layer]) {
		if (!l->mapped || l->canvas_box.has_value()) {
			continue; // on the plane: canvas_surface_at finds it
		}
		double sx = 0, sy = 0;
		wlr_surface *surface = wlr_layer_surface_v1_surface_at(
			l->wlr, ox - l->geo.x, oy - l->geo.y, &sx, &sy);
		if (surface != nullptr) {
			return LayerHit{
				.layer = l.get(),
				.surface = surface,
				.sx = sx,
				.sy = sy,
			};
		}
	}
	return std::nullopt;
}

bool Output::has_wallpaper() const {
	for (const std::unique_ptr<LayerSurface> &layer :
			layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
		if (layer->mapped) {
			return true;
		}
	}
	return false;
}

void Server::refocus_layers() {
	// The topmost mapped exclusively keyboard-interactive surface, overlay
	// before top, owns the keyboard.
	for (std::size_t i = 0; i < 2; i++) {
		for (const std::unique_ptr<Output> &output : outputs) {
			for (const std::unique_ptr<LayerSurface> &layer :
					output->layers[layer_order[i]]) {
				if (!layer->mapped || !layer->holds_keyboard_exclusively()) {
					continue;
				}
				focused_layer = layer.get();
				focus_keyboard(layer->wlr->surface);
				return;
			}
		}
	}

	LayerSurface *focused = focused_layer;
	if (focused == nullptr || focused->mapped) {
		// A still-mapped on-demand surface keeps the keyboard it got on
		// map or by a click.
		return;
	}
	focused_layer = nullptr;
	View *view = focused_view;
	if (view != nullptr && view->mapped) {
		focus_keyboard(view->wlr->base->surface);
	} else {
		focus_keyboard(nullptr);
	}
}

void Server::init_layers() {
	layer_shell = wlr_layer_shell_v1_create(display, 4);
	if (layer_shell == nullptr) {
		wlr_log(WLR_ERROR, "failed to create the layer shell");
		return;
	}
	new_layer_surface_.connect<&Server::handle_new_layer_surface>(
		layer_shell->events.new_surface, this);
}

void Server::finish_layers() {
	new_layer_surface_.disconnect();
}

void Server::handle_new_layer_surface(wlr_layer_surface_v1 *ls) {
	Output *output = ls->output != nullptr
		? static_cast<Output *>(ls->output->data)
		: output_under_cursor();
	if (output == nullptr) {
		wlr_log(WLR_ERROR, "layer surface with no output to place it on");
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	ls->output = output->wlr;

	// Head of a layer vector is the topmost (newest) surface.
	auto &vec = output->layers[ls->pending.layer];
	vec.insert(vec.begin(), std::make_unique<LayerSurface>(*this, *output, ls));
}
