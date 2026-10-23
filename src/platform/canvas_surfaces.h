#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "core/util.h"

class LayerSurface;
class Server;

// The surfaces plugins put on the plane (docs/intents/platform/ipc.md): where
// each was asked to be, by name, and the order they stack in. The one writer
// of both, so a place and the surfaces carrying it never disagree.
class CanvasSurfaces {
public:
	explicit CanvasSurfaces(Server &server) : server_(server) {}
	CanvasSurfaces(const CanvasSurfaces &) = delete;
	CanvasSurfaces &operator=(const CanvasSurfaces &) = delete;

	// Puts every surface of that name at `box` and remembers it under the name,
	// so a surface that maps later, or again after a shell reload, lands there.
	void place(std::string_view name, wlr_box box);
	// The same for one surface and its name, as a drag moves it.
	void place(LayerSurface &layer, wlr_box box);
	// Takes every surface of that name back to its screen and forgets the place.
	void unplace(std::string_view name);
	// Where `name` was asked to be; nullopt when it was not, or was unplaced.
	std::optional<wlr_box> place_of(std::string_view name) const;

	// A mapped surface that belongs on the plane joins the stack, on top; one
	// already in it stays where it is.
	void add(LayerSurface &layer);
	// Off the stack, whether it unmapped, was unplaced or is destroyed.
	void remove(LayerSurface &layer);
	// Above the others on the plane.
	void raise(LayerSurface &layer);
	// The surfaces on the plane, bottom first: every output draws and
	// hit-tests them in this order, whichever output each was made on.
	std::span<LayerSurface *const> bottom_up() const { return stack_; }

	// Every layer surface carrying the name, on any output: one, or briefly
	// two while a reloading shell replaces it.
	std::vector<LayerSurface *> named(std::string_view name) const;

private:
	void remember(std::string_view name, wlr_box box);

	Server &server_;
	std::map<std::string, wlr_box, std::less<>> places_;
	std::vector<LayerSurface *> stack_;
};
