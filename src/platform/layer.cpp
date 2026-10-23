#include "platform/layer.h"

#include <algorithm>
#include <iterator>
#include <optional>
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

// One axis of a box: where it starts and how far it runs.
struct Span {
	int start;
	int length;
};

// Where a layer surface goes along one axis of `bounds`: a desired length of
// 0 fills between the margins, a surface anchored to both ends or to
// neither is centred. Same math as wlr_scene_layer_surface_v1_configure.
Span place_on_axis(Span bounds, int desired, int margin_start, int margin_end,
		bool anchored_start, bool anchored_end) {
	if (desired == 0) {
		return {bounds.start + margin_start, bounds.length - (margin_start + margin_end)};
	}
	if (anchored_start == anchored_end) {
		return {bounds.start + bounds.length / 2 - desired / 2, desired};
	}
	if (anchored_start) {
		return {bounds.start + margin_start, desired};
	}
	return {bounds.start + bounds.length - desired - margin_end, desired};
}

// Takes a zone and the margin on its side off one axis of the usable box.
void take_from_axis(Span &usable, int zone, int margin, bool from_start) {
	int taken = zone + margin;
	if (from_start) {
		usable.start += taken;
	}
	usable.length = std::max(usable.length - taken, 0);
}

// Takes the surface's exclusive zone off the edge it reserves.
void reserve_zone(wlr_layer_surface_v1 &ls, wlr_box &usable) {
	const wlr_layer_surface_v1_state &state = ls.current;
	if (!ls.surface->mapped || state.exclusive_zone <= 0) {
		return;
	}
	Span x{usable.x, usable.width};
	Span y{usable.y, usable.height};
	switch (wlr_layer_surface_v1_get_exclusive_edge(&ls)) {
	case WLR_EDGE_TOP:
		take_from_axis(y, state.exclusive_zone, state.margin.top, true);
		break;
	case WLR_EDGE_BOTTOM:
		take_from_axis(y, state.exclusive_zone, state.margin.bottom, false);
		break;
	case WLR_EDGE_LEFT:
		take_from_axis(x, state.exclusive_zone, state.margin.left, true);
		break;
	case WLR_EDGE_RIGHT:
		take_from_axis(x, state.exclusive_zone, state.margin.right, false);
		break;
	default:
		return;
	}
	usable = {x.start, y.start, x.length, y.length};
}

// Positions and sizes one layer surface, and takes its exclusive zone off
// `usable`.
void layer_configure(LayerSurface &layer, const wlr_box &full, wlr_box &usable) {
	wlr_layer_surface_v1 *ls = layer.wlr;
	const wlr_layer_surface_v1_state &state = ls->current;

	// Zone -1: cover the whole output, ignoring other exclusive zones.
	const wlr_box &bounds = state.exclusive_zone == -1 ? full : usable;
	auto anchored = [&state](uint32_t edge) { return (state.anchor & edge) != 0; };

	Span x = place_on_axis({bounds.x, bounds.width}, static_cast<int>(state.desired_width),
		state.margin.left, state.margin.right,
		anchored(ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT), anchored(ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT));
	Span y = place_on_axis({bounds.y, bounds.height}, static_cast<int>(state.desired_height),
		state.margin.top, state.margin.bottom,
		anchored(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP), anchored(ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM));
	layer.geo = {x.start, y.start, x.length, y.length};

	// configure() asserts on surfaces that have not done their initial commit.
	if (ls->initialized) {
		wlr_layer_surface_v1_configure(ls, layer.geo.width, layer.geo.height);
	}
	reserve_zone(*ls, usable);
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
	layer.server.canvas_surfaces.remove(layer);
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
	if (std::optional<wlr_box> asked = server.canvas_surfaces.place_of(name())) {
		canvas_box = asked;
	}
	mapped = true;
	if (belongs_on_canvas() || canvas_box.has_value()) {
		server.canvas_surfaces.add(*this);
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
	if (above_windows && wants_keyboard()) {
		server.focus.offer_keyboard(*this, wlr->surface);
	}
	server.focus.refocus_layers();
}

void LayerSurface::handle_unmap() {
	mapped = false;
	leave_canvas(*this);
	if (attached_) {
		// Release the exclusive zone the surface was reserving.
		output->arrange_layers();
	}
	server.focus.refocus_layers();
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
	if (server.focus.focused_layer() == this && !wants_keyboard()) {
		server.focus.return_keyboard(); // it stopped asking for the keyboard
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

	server.focus.forget(*this);
	if (attached_) {
		output->arrange_layers();
	}
	server.focus.refocus_layers();
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
	// The client is told its new size; one that has not made its first
	// commit is told by the arrange that commit runs (configure() asserts
	// before it).
	if (resized && wlr->initialized) {
		geo = box;
		wlr_layer_surface_v1_configure(wlr, static_cast<uint32_t>(box.width),
			static_cast<uint32_t>(box.height));
	}
	if (mapped) {
		server.canvas_surfaces.add(*this);
	}
	// Where it was and where it is now: both may be on any output.
	server.outputs.damage_all();
}

void LayerSurface::unplace() {
	server.input.grab.forget(*this);
	canvas_box.reset();
	if (!belongs_on_canvas()) {
		server.canvas_surfaces.remove(*this); // back on its screen
	}
	output->arrange_layers();
	server.outputs.damage_all();
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

// The layer surfaces die with the output; destroying each wlroots object
// runs its own destroy handler, which takes it out of these vectors. A
// surface on the plane is not the output's: another output takes it.
void Output::release_layers() {
	Output *heir = nullptr;
	for (const std::unique_ptr<Output> &other : server.outputs) {
		if (other.get() != this) {
			heir = other.get();
			break;
		}
	}
	std::vector<LayerSurface *> doomed, kept;
	for (const std::vector<std::unique_ptr<LayerSurface>> &vec : layers) {
		for (const std::unique_ptr<LayerSurface> &layer : vec) {
			bool on_plane = layer->belongs_on_canvas() || layer->canvas_box.has_value();
			(heir != nullptr && on_plane ? kept : doomed).push_back(layer.get());
		}
	}
	for (LayerSurface *layer : kept) {
		layer->rehome(*heir);
	}
	for (LayerSurface *layer : doomed) {
		layer->attached_ = false;
		wlr_layer_surface_v1_destroy(layer->wlr);
	}
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
	visibility_changed();
}

// A surface a plugin put on the plane, under an output-local point, the
// topmost first: the point is taken to the canvas first, so the hit
// follows the camera, and every output finds every such surface.
std::optional<LayerHit> Output::canvas_surface_at(double ox, double oy) const {
	Point on_plane = local_to_canvas(ox, oy);
	for (LayerSurface *l : std::views::reverse(server.canvas_surfaces.bottom_up())) {
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

LayerSurface *exclusive_keyboard_holder(const Server &server) {
	for (zwlr_layer_shell_v1_layer layer :
			{ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP}) {
		for (const std::unique_ptr<Output> &output : server.outputs) {
			for (const std::unique_ptr<LayerSurface> &surface : output->layers[layer]) {
				if (surface->mapped && surface->holds_keyboard_exclusively()) {
					return surface.get();
				}
			}
		}
	}
	return nullptr;
}
