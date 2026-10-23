#include "platform/canvas_surfaces.h"

#include <algorithm>
#include <iterator>
#include <memory>

#include "core/server.h"
#include "platform/layer.h"
#include "platform/output.h"

void CanvasSurfaces::remember(std::string_view name, wlr_box box) {
	auto found = places_.find(name);
	if (found != places_.end()) {
		found->second = box;
		return;
	}
	places_.emplace(std::string{name}, box);
}

void CanvasSurfaces::place(std::string_view name, wlr_box box) {
	remember(name, box);
	for (LayerSurface *layer : named(name)) {
		layer->place(box);
	}
}

void CanvasSurfaces::place(LayerSurface &layer, wlr_box box) {
	remember(layer.name(), box);
	layer.place(box);
}

void CanvasSurfaces::unplace(std::string_view name) {
	if (auto found = places_.find(name); found != places_.end()) {
		places_.erase(found);
	}
	for (LayerSurface *layer : named(name)) {
		layer->unplace();
	}
}

std::optional<wlr_box> CanvasSurfaces::place_of(std::string_view name) const {
	auto found = places_.find(name);
	if (found == places_.end()) {
		return std::nullopt;
	}
	return found->second;
}

void CanvasSurfaces::add(LayerSurface &layer) {
	if (std::ranges::find(stack_, &layer) == stack_.end()) {
		stack_.push_back(&layer);
	}
}

void CanvasSurfaces::remove(LayerSurface &layer) {
	std::erase(stack_, &layer);
}

void CanvasSurfaces::raise(LayerSurface &layer) {
	auto it = std::ranges::find(stack_, &layer);
	if (it == stack_.end() || std::next(it) == stack_.end()) {
		return;
	}
	stack_.erase(it);
	stack_.push_back(&layer);
	layer.damage();
}

std::vector<LayerSurface *> CanvasSurfaces::named(std::string_view name) const {
	std::vector<LayerSurface *> found;
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		for (const auto &surfaces : output->layers) {
			for (const std::unique_ptr<LayerSurface> &layer : surfaces) {
				if (layer->name() == name) {
					found.push_back(layer.get());
				}
			}
		}
	}
	return found;
}
