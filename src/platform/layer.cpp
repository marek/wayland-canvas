#include "platform/layer.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <string_view>

#include "platform/output.h"
#include "core/config.h"
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
	for (std::vector<std::unique_ptr<LayerSurface>> &vec : layer.output->layers) {
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

// Off the plane's stacking order, and out of any drag the pointer holds it in.
void leave_canvas(LayerSurface &layer) {
	std::erase(layer.server.canvas_stack, &layer);
	layer.server.input.grab.forget(layer);
}

// The namespaces of the shell's overview strips. Keep them in sync with
// plugins/canvas.toolbar and plugins/canvas.apps
// (docs/intents/shell/toolbar.md, app-bar.md).
constexpr std::string_view overview_strip_namespaces[] = {
	"wayland-canvas-toolbar",
	"wayland-canvas-app-bar",
};

bool is_strip_namespace(std::string_view name) {
	return std::ranges::find(overview_strip_namespaces, name)
		!= std::end(overview_strip_namespaces);
}

} // namespace

LayerSurface::LayerSurface(Server &server, Output &output, wlr_layer_surface_v1 *wlr)
		: server(server), output(&output), wlr(wlr),
		on_canvas_(name().starts_with(config::canvas_surface_prefix)),
		overview_strip_(is_strip_namespace(name())) {
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

void LayerSurface::handle_map() {
	// A placement asked for before this surface existed applies as it
	// maps, so the ask can never race the surface.
	auto found = server.canvas_places.find(name());
	if (found != server.canvas_places.end()) {
		canvas_box = found->second;
	}
	mapped = true;
	if (belongs_on_canvas() || canvas_box.has_value()) {
		server.canvas_stack.push_back(this); // the newest on top
	}
	if (attached_) {
		// The surface's exclusive zone (if any) takes effect now.
		output->arrange_layers();
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
	leave_canvas(*this);
	if (attached_) {
		// Release the exclusive zone the surface was reserving.
		output->arrange_layers();
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
			output->layers[ls->current.layer].insert(
				output->layers[ls->current.layer].begin(), std::move(owned));
		}
	}
	if (server.focused_layer == this && !wants_keyboard()) {
		server.return_keyboard(); // it stopped asking for the keyboard
	}
	output->arrange_layers();
}

void LayerSurface::handle_destroy() {
	map_.disconnect();
	unmap_.disconnect();
	commit_.disconnect();
	destroy_.disconnect();
	leave_canvas(*this);

	// Off the output's lists before anything below looks at them.
	std::unique_ptr<LayerSurface> owned = detach(*this);

	if (server.focused_layer == this) {
		server.focused_layer = nullptr;
	}
	if (attached_) {
		output->arrange_layers();
	}
	server.refocus_layers();
} // `owned` releasing destroys this

std::string LayerSurface::name() const {
	return wlr->namespace_ != nullptr ? std::string{wlr->namespace_} : std::string{};
}

bool LayerSurface::belongs_on_canvas() const {
	return on_canvas_;
}

bool LayerSurface::is_overview_strip() const {
	return overview_strip_;
}

void LayerSurface::place(wlr_box box) {
	bool resized = !canvas_box.has_value() || canvas_box->width != box.width
		|| canvas_box->height != box.height;
	canvas_box = box;
	server.canvas_places[name()] = box;
	// The client is told its new size; one that has not made its first
	// commit is told by the arrange that commit runs (configure() asserts
	// before it).
	if (resized && wlr->initialized) {
		geo = box;
		wlr_layer_surface_v1_configure(wlr, static_cast<uint32_t>(box.width),
			static_cast<uint32_t>(box.height));
	}
	if (mapped && std::ranges::find(server.canvas_stack, this) == server.canvas_stack.end()) {
		server.canvas_stack.push_back(this);
	}
	// Where it was and where it is now: both may be on any output.
	server.damage_all();
}

void LayerSurface::unplace() {
	server.input.grab.forget(*this);
	canvas_box.reset();
	if (!belongs_on_canvas()) {
		std::erase(server.canvas_stack, this); // back on its screen
	}
	output->arrange_layers();
	server.damage_all();
}

void LayerSurface::damage() {
	if (!canvas_box.has_value()) {
		output->damage();
		return;
	}
	const wlr_box &box = *canvas_box;
	// The committed size may run ahead of the rectangle mid-resize.
	Rect drawn{double(box.x), double(box.y),
		double(std::max(box.width, wlr->surface->current.width)),
		double(std::max(box.height, wlr->surface->current.height))};
	for (const std::unique_ptr<Output> &other : server.outputs) {
		if (drawn.intersects(other->viewport())) {
			other->damage();
		}
	}
}

void LayerSurface::rehome(Output &to) {
	std::unique_ptr<LayerSurface> owned = detach(*this);
	if (owned == nullptr) {
		return;
	}
	output = &to;
	wlr->output = to.wlr;
	std::vector<std::unique_ptr<LayerSurface>> &vec = to.layers[wlr->current.layer];
	vec.insert(vec.begin(), std::move(owned));
	to.arrange_layers();
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
				if (layer->belongs_on_canvas() || layer->canvas_box.has_value()) {
					// configure() asserts before the first commit
					if (!layer->wlr->initialized) {
						continue;
					}
					// Unplaced, the client is left to its own size so it can
					// map and be found by name, and nothing of it is drawn
					// until the plugin says where on the plane it goes;
					// placed, it is sized by that rectangle, not by the output.
					wlr_box box = layer->canvas_box.value_or(wlr_box{});
					layer->geo = box;
					wlr_layer_surface_v1_configure(layer->wlr,
						static_cast<uint32_t>(box.width), static_cast<uint32_t>(box.height));
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

// A surface a plugin put on the plane, under an output-local point, the
// topmost first: the point is taken to the canvas first, so the hit
// follows the camera, and every output finds every such surface.
std::optional<LayerHit> Output::canvas_surface_at(double ox, double oy) const {
	Point on_plane = local_to_canvas(ox, oy);
	for (LayerSurface *l : std::views::reverse(server.canvas_stack)) {
		if (!l->mapped || !l->canvas_box.has_value()) {
			continue;
		}
		// The surface as it committed: its own size and input region.
		double in_x = 0, in_y = 0;
		wlr_surface *surface = wlr_surface_surface_at(l->wlr->surface,
			on_plane.x - l->canvas_box->x, on_plane.y - l->canvas_box->y,
			&in_x, &in_y);
		if (surface != nullptr) {
			return LayerHit{.layer = l, .surface = surface, .sx = in_x, .sy = in_y};
		}
	}
	return std::nullopt;
}

std::optional<LayerHit> Output::layer_surface_at(int layer, double ox, double oy) const {
	for (const std::unique_ptr<LayerSurface> &l : layers[layer]) {
		if (!l->mapped || l->canvas_box.has_value() || l->belongs_on_canvas()) {
			continue; // the plane's: canvas_surface_at finds it, or nothing does
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
	return_keyboard();
}

void Server::return_keyboard() {
	focused_layer = nullptr;
	View *view = focused_view;
	focus_keyboard(view != nullptr && view->mapped ? view->surface() : nullptr);
}

void Server::raise_canvas_surface(LayerSurface &layer) {
	auto it = std::ranges::find(canvas_stack, &layer);
	if (it == canvas_stack.end() || std::next(it) == canvas_stack.end()) {
		return;
	}
	canvas_stack.erase(it);
	canvas_stack.push_back(&layer);
	layer.damage();
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
