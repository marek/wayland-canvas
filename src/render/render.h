#pragma once

#include <array>
#include <cstddef>
// libstdc++ 16's <inplace_vector> uses the C++26-deprecated is_trivial_v internally.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <inplace_vector>
#pragma clang diagnostic pop
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "canvas/desktop.h"
#include "canvas/items.h"

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

// Everything one render target's frame is drawn with: an output's on-screen
// frame, or an offscreen capture of a rectangle of that output (see
// Painter::render_area).
struct RenderCtx {
	Output *output;
	wlr_render_pass *pass;
	Camera cam; // output->camera(): quantized, same as hit testing
	Rect viewport; // visible canvas rectangle
	double left, top; // canvas coords shown at output-local (0, 0)
	float scale;
	// What title bars are drawn for: the output's scale times the zoom the
	// camera rests at, so they stay sharp on a scaled desktop.
	double deco_scale;
	wl_output_transform transform; // maps target-local logical to buffer pixels
	wl_output_transform out_transform; // what textures are counter-rotated by
	int tw, th; // buffer resolution, in transform order
	// The render target: an output-local logical rectangle — the whole output
	// for an on-screen frame, the captured area for an offscreen one.
	double origin_x = 0, origin_y = 0;
	int vw = 0, vh = 0;
	// Whether the compositor's own UI is drawn: true for an on-screen frame;
	// false for an offscreen capture, which shows the desktop, not the
	// picker or toolbar that asked for it.
	bool show_ui = true;
	bool overview;
	std::vector<DrawnSurface> *drawn; // Painter's reused per-frame buffer
	std::span<HelpSlot> help_cache; // Painter's help-line texture cache
	unsigned *help_clock;
};

// A desktop tab's label texture and its size, drawn at `scale`.
struct DesktopLabel {
	wlr_texture *texture = nullptr; // null when the text failed to render
	int w = 0, h = 0; // logical px
};

// A canvas item's card texture, drawn at the scale it was rendered at.
struct ItemTexture {
	wlr_texture *texture = nullptr; // null when the card failed to render
	int w = 0, h = 0; // logical px (canvas px)
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
	// Drops the cached texture so the next frame redraws it in the new palette.
	void theme_changed();
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
	// Draws `local_area` (output-local logical) of what the output shows now
	// into an offscreen ARGB8888 `target` of exactly the area at the output's
	// scale, untransformed — the same pixels an on-screen frame of that area
	// holds. False (logged) when the pass cannot begin or the area is empty.
	// Surfaces drawn get no enter or frame feedback: the output still owns that.
	bool render_area(Output &output, wlr_box local_area, wlr_buffer *target);
	void theme_changed(); // drops the help-line and tab-label textures
	void finish(); // releases cached textures before the renderer goes

	// Regenerates the cached label when the desktop's number or scale, the
	// output scale, or the palette changed; `on_accent` picks the variant
	// for the target tab's accent fill, in the contrasting accent text
	// colour instead of the panel text colour.
	DesktopLabel desktop_label(const Desktop &desktop, float scale, bool on_accent);
	// The tab above a desktop frame and its Tile and Abandon buttons; nullopt when the
	// tab is off-screen.
	std::optional<TabLayout> overview_tab_layout(const Output &output,
		const Desktop &desktop) const;

	// The item's card at `scale` (the output's scale times the zoom it is
	// shown at). Re-rendered when its content changed, or once `settled`
	// when the shown scale changed; a moving camera stretches the cached
	// card instead of re-rendering every frame.
	ItemTexture item_texture(const Item &item, float scale, bool settled);

private:
	static constexpr std::size_t help_slots = 4;

	// The context one render target's frame is drawn with.
	RenderCtx make_ctx(Output &output, wlr_render_pass *pass);
	// Everything from the canvas clear to the drag icon, into ctx's target.
	void draw_scene(const RenderCtx &ctx);

	// One slot of the desktop-label cache: one texture per fill the label
	// sits on — the panel of an ordinary tab, the accent of the target.
	struct LabelSlot {
		DesktopId id{};
		TexturePtr texture;
		TexturePtr accent_texture;
		int number = 0, w = 0, h = 0;
		double desktop_scale = 1; // the scale the label names
		float scale = 0;
	};

	LabelSlot *label_slot(DesktopId id);
	const LabelSlot *label_slot(DesktopId id) const;

	Server &server_;
	std::vector<DrawnSurface> drawn_; // surfaces drawn this frame, reused
	std::array<HelpSlot, help_slots> help_cache_{}; // help-line texture, one per scale
	std::vector<LabelSlot> label_cache_; // desktop tab labels, dropped with their desktops
	unsigned help_clock_ = 0; // round-robin over help_cache_

	// One slot of the item-card cache.
	struct ItemSlot {
		std::string id;
		TexturePtr texture;
		std::string text;
		uint32_t color = 0;
		int w = 0, h = 0;
		float scale = 0;
	};
	std::vector<ItemSlot> item_cache_; // item cards, dropped with their items

};
