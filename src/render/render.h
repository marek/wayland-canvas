#pragma once

#include <array>
#include <cstddef>
// libstdc++ 16's <inplace_vector> uses the C++26-deprecated is_trivial_v internally.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <inplace_vector>
#pragma clang diagnostic pop
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "canvas/desktop.h"
#include "core/util.h"

class Output;
class Server;
class View;

// Overview UI geometry in output-local logical coordinates; input.cpp hit-tests the same
// boxes render.cpp draws.

// The tab above a desktop frame and its Tile and Abandon buttons; nullopt when the tab
// is off-screen.
struct TabLayout {
	wlr_box tab{}, tile_button{}, abandon_button{};
};

inline constexpr std::size_t shelf_max = 64;

struct ShelfItem {
	View *view = nullptr;
	wlr_box box{}; // thumbnail
};
using Shelf = std::inplace_vector<ShelfItem, shelf_max>;

// Minimized windows shelf along the bottom of an overview output.
Shelf overview_shelf_layout(const Output &output);

// Surfaces drawn this frame: they need enter/presentation feedback, and frame_done when
// their output owns them. Painter's reused per-frame buffer.
struct DrawnSurface {
	wlr_surface *surface = nullptr;
	bool frame_done = false;
};

// One scale-keyed slot of the help-line texture cache, reused by Painter.
struct HelpSlot {
	TexturePtr texture;
	int w = 0, h = 0;
	float scale = 0;
};

// A desktop tab's label texture and its size, drawn at `scale`.
struct DesktopLabel {
	wlr_texture *texture = nullptr; // null when the text failed to render
	int w = 0, h = 0; // logical px
};

// Transient banner at the bottom of every output.
class Hint {
public:
	explicit Hint(Server &server);
	~Hint();
	Hint(const Hint &) = delete;
	Hint &operator=(const Hint &) = delete;

	// Shows `text` for about 2 s; empty clears.
	void set(std::string_view text);
	void finish(); // releases the timer and texture before the display and renderer go

	std::string text;
	TexturePtr texture; // drawn for `text` at `scale`
	int w = 0, h = 0;
	float scale = 0;

private:
	void clear();

	Server &server_;
	EventSourcePtr timer_;
};

// Draws an output's frame: the canvas, windows, desktop frames and the overview UI.
class Painter {
public:
	explicit Painter(Server &server);
	~Painter();
	Painter(const Painter &) = delete;
	Painter &operator=(const Painter &) = delete;

	void render(Output &output);
	void finish(); // releases cached textures before the renderer goes

	// Regenerates the cached label when its desktop's number or the scale changed.
	DesktopLabel desktop_label(DesktopId id, int number, float scale);
	// The tab above a desktop frame and its Tile and Abandon buttons; nullopt when the
	// tab is off-screen.
	std::optional<TabLayout> overview_tab_layout(const Output &output,
		const Desktop &desktop) const;

private:
	static constexpr std::size_t help_slots = 4;

	// One slot of the desktop-label cache.
	struct LabelSlot {
		DesktopId id{};
		TexturePtr texture;
		int number = 0, w = 0, h = 0;
		float scale = 0;
	};

	LabelSlot *label_slot(DesktopId id);
	const LabelSlot *label_slot(DesktopId id) const;

	Server &server_;
	std::vector<DrawnSurface> drawn_; // surfaces drawn this frame, reused
	std::array<HelpSlot, help_slots> help_cache_{}; // help-line texture, one per scale
	std::vector<LabelSlot> label_cache_; // desktop tab labels, dropped with their desktops
	unsigned help_clock_ = 0; // round-robin over help_cache_
};
