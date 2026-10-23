#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"
#include "core/server.h"
#include "platform/ipc_methods.h"
#include "platform/ipc_params.h"
#include "platform/ipc_server.h"
#include "platform/layer.h"
#include "platform/output.h"

namespace {

// The request of a surface.move or surface.resize waits on the button's release.
constexpr std::string_view drag_wait = "surface.drag";

// Every layer surface carrying a plugin's name, on any output: one, or
// briefly two while a reloading shell replaces it.
std::vector<LayerSurface *> named_surfaces(Server &server, std::string_view name) {
	std::vector<LayerSurface *> found;
	for (const std::unique_ptr<Output> &output : server.outputs) {
		for (auto &surfaces : output->layers) {
			for (const std::unique_ptr<LayerSurface> &layer : surfaces) {
				if (layer->name() == name) {
					found.push_back(layer.get());
				}
			}
		}
	}
	return found;
}

// The `edges` of a `surface.resize`: a list of edge names, as WLR_EDGE bits.
std::optional<uint32_t> edge_bits(JsonObject *params) {
	JsonArray *list = params != nullptr
		? node_array(json_object_get_member(params, "edges")) : nullptr;
	if (list == nullptr) {
		return std::nullopt;
	}
	constexpr std::array names = {
		std::pair{std::string_view{"left"}, WLR_EDGE_LEFT},
		std::pair{std::string_view{"right"}, WLR_EDGE_RIGHT},
		std::pair{std::string_view{"top"}, WLR_EDGE_TOP},
		std::pair{std::string_view{"bottom"}, WLR_EDGE_BOTTOM},
	};
	uint32_t edges = 0;
	for (guint i = 0; i < json_array_get_length(list); i++) {
		const char *edge = node_string(json_array_get_element(list, i));
		auto known = std::ranges::find_if(names, [edge](const auto &entry) {
			return edge != nullptr && entry.first == edge;
		});
		if (known == names.end()) {
			return std::nullopt;
		}
		edges |= known->second;
	}
	if (edges == 0) {
		return std::nullopt;
	}
	return edges;
}

IpcServer::Reply surface_place(Server &server, JsonObject *params) {
	constexpr std::string_view usage =
		"params: surface (a layer surface's name), x, y, width, height";
	std::expected<std::optional<std::string>, std::string> name =
		opt_string(params, "surface");
	if (!name.has_value() || !name->has_value() || (*name)->empty()) {
		return std::unexpected(std::string{usage});
	}
	std::expected<std::optional<long long>, std::string> x = opt_int(params, "x");
	std::expected<std::optional<long long>, std::string> y = opt_int(params, "y");
	std::expected<std::optional<long long>, std::string> width =
		opt_int(params, "width");
	std::expected<std::optional<long long>, std::string> height =
		opt_int(params, "height");
	for (const auto &value : {x, y, width, height}) {
		if (!value.has_value()) {
			return std::unexpected(value.error());
		}
		if (!value->has_value()) {
			return std::unexpected(std::string{usage});
		}
	}
	if (*(*width) < 1 || *(*height) < 1) {
		return std::unexpected(std::string{"a surface needs an area to be placed in"});
	}
	wlr_box box{static_cast<int>(*(*x)), static_cast<int>(*(*y)),
		static_cast<int>(*(*width)), static_cast<int>(*(*height))};
	// Remembered by name: a surface that has not mapped yet takes it when
	// it does, and a shell that reloads finds its places again.
	server.canvas_places[*(*name)] = box;
	for (LayerSurface *surface : named_surfaces(server, *(*name))) {
		surface->place(box);
	}
	return std::string{"true"};
}

IpcServer::Reply surface_unplace(Server &server, JsonObject *params) {
	std::expected<std::optional<std::string>, std::string> name =
		opt_string(params, "surface");
	if (!name.has_value() || !name->has_value() || (*name)->empty()) {
		return std::unexpected(std::string{"params: surface (a layer surface's name)"});
	}
	server.canvas_places.erase(*(*name));
	for (LayerSurface *surface : named_surfaces(server, *(*name))) {
		surface->unplace();
	}
	return std::string{"true"};
}

// What `surface.move` and `surface.resize` answer at the release: the
// rectangle the surface ended at, or an error when the drag lost its surface.
std::expected<std::string, std::string> drag_answer(std::optional<wlr_box> box) {
	if (!box.has_value()) {
		return std::unexpected(std::string{"the drag ended with its surface gone"});
	}
	return object_text([&](JsonBuilder *builder) {
		add_rect_members(builder, box->x, box->y, box->width, box->height);
	});
}

// Hands the press held on a plugin's surface to a compositor drag; the
// release answers.
IpcServer::Reply surface_drag(IpcServer &ipc, Server &server, JsonObject *params, bool resize) {
	std::string_view usage = resize
		? "params: surface (a layer surface's name), edges (a list of left, "
			"right, top, bottom), minWidth and minHeight (optional)"
		: "params: surface (a layer surface's name)";
	std::expected<std::optional<std::string>, std::string> name =
		opt_string(params, "surface");
	if (!name.has_value() || !name->has_value() || (*name)->empty()) {
		return std::unexpected(std::string{usage});
	}
	uint32_t edges = 0;
	int min_w = 1, min_h = 1;
	if (resize) {
		std::optional<uint32_t> bits = edge_bits(params);
		std::expected<std::optional<long long>, std::string> wide =
			opt_int(params, "minWidth");
		std::expected<std::optional<long long>, std::string> tall =
			opt_int(params, "minHeight");
		if (!bits.has_value() || !wide.has_value() || !tall.has_value()) {
			return std::unexpected(std::string{usage});
		}
		edges = *bits;
		min_w = static_cast<int>(wide->value_or(1));
		min_h = static_cast<int>(tall->value_or(1));
	}
	// The surface of that name the pointer is pressed on: the press the
	// plugin is answering, which the drag continues.
	LayerSurface *held = nullptr;
	for (LayerSurface *surface : named_surfaces(server, *(*name))) {
		if (surface->mapped && server.input.button_held_on(surface->wlr->surface)) {
			held = surface;
		}
	}
	if (held == nullptr) {
		return std::unexpected(std::string{"no button is held on that surface"});
	}
	if (!held->canvas_box.has_value()) {
		return std::unexpected(std::string{"that surface is not on the plane"});
	}
	Output *output = server.output_at(server.cursor->x, server.cursor->y);
	if (server.input.grab.active() || ipc.waiting(drag_wait) || output == nullptr) {
		return std::unexpected(std::string{"the pointer is already in a drag"});
	}
	server.input.grab.begin_surface_drag(*held, *output, server.input.press_lx(),
		server.input.press_ly(), edges, min_w, min_h);
	return IpcServer::Wait{drag_wait};
}

} // namespace

void register_surface_methods(IpcServer &ipc, Server &server) {
	ipc.register_method("surface.place",
		[&server](JsonObject *params) { return surface_place(server, params); });
	ipc.register_method("surface.unplace",
		[&server](JsonObject *params) { return surface_unplace(server, params); });
	ipc.register_method("surface.move", [&ipc, &server](JsonObject *params) {
		return surface_drag(ipc, server, params, false);
	});
	ipc.register_method("surface.resize", [&ipc, &server](JsonObject *params) {
		return surface_drag(ipc, server, params, true);
	});
}

void answer_surface_drag(IpcServer &ipc, std::optional<wlr_box> box) {
	ipc.answer(drag_wait, drag_answer(box));
}
