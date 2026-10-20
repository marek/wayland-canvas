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
#include "input/gesture.h"
#include "render/text_cache.h"

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

// The overview's bottom stack on an output, from its bottom edge (or the top of
// the highest strip anchored to it) up: the help line, the row a hint takes
// above it, and the shelf's bottom above both. Output-local logical px. What is
// drawn and what takes the pointer read this one layout.
struct OverviewBottom {
	wlr_box help{}; // the help line, shrunk to fit a narrow output; empty without text
	wlr_box hint{}; // the hint's row, as wide as the output
	int shelf_bottom = 0; // the y the shelf's thumbnails end at
};

// `scale` is what the help line's text is measured at.
OverviewBottom overview_bottom_layout(const Output &output, float scale);

// Minimized windows shelf along the bottom of an overview output, above
// overview_bottom_layout()'s hint row.
Shelf overview_shelf_layout(const Output &output);

// Surfaces drawn this frame: they need enter/presentation feedback, and frame_done when
// their output owns them. Painter's reused per-frame buffer.
struct DrawnSurface {
	wlr_surface *surface = nullptr;
	bool frame_done = false;
};

// The overview's help line for the gesture options in force: the swipe
// preset's gestures, or only the keys when no preset binds any.
const char *overview_help_text(const GestureOptions &options);

// A desktop tab's label at `scale`, from the text cache; `on_accent` is the
// target tab's variant, in the contrasting accent text colour instead of the
// panel text colour.
CachedText desktop_label(Server &server, const Desktop &desktop, float scale, bool on_accent);

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
	// Whether this is an offscreen capture: it shows what the output shows,
	// minus the compositor's UI that asked for it (the capture UI, which a
	// shot or a shared screen never shows).
	bool offscreen = false;
	bool overview;
	std::vector<DrawnSurface> *drawn; // Painter's reused per-frame buffer
};

// Screen-space rect (output-local logical px), clipped to the render target;
// empty rects are skipped. `color` is premultiplied.
void draw_logical_rect(const RenderCtx &ctx, double x, double y, double w, double h,
	wlr_render_color color);
// A cairo texture of ours at (x, y) of size (w, h), output-local logical px;
// `src_frac` crops its source horizontally (0..1).
void draw_ui_texture(const RenderCtx &ctx, wlr_texture *texture, double x, double y,
	double w, double h, double src_frac);

// Transient banner at the bottom of every output.
class Hint {
public:
	explicit Hint(Server &server);
	~Hint();
	Hint(const Hint &) = delete;
	Hint &operator=(const Hint &) = delete;

	// Shows `text` for about 2 s; empty clears. The text already showing
	// only has its 2 s restarted.
	void set(std::string_view text);
	// Restarts the 2 s of the text showing; nothing when none shows.
	void keep();
	void finish(); // releases the timer before the display goes

	std::string text;

private:
	void clear();
	void restart_timer();

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
	// Draws `view` alone — title bar, border, content, popups, nothing over
	// it, minimized or not — into an offscreen ARGB8888 `target` of
	// view_device_size(view, scale): the frame's corner at (0, 0), zoom 1,
	// `scale` device pixels per logical pixel, over transparent. The same
	// draw_view an on-screen frame uses. False (logged) when the pass cannot
	// begin, the view is not mapped, the session is locked or no output exists.
	bool render_view(View &view, float scale, wlr_buffer *target);

	// The tab above a desktop frame and its Tile and Abandon buttons; nullopt when the
	// tab is off-screen.
	std::optional<TabLayout> overview_tab_layout(const Output &output,
		const Desktop &desktop) const;

private:
	// The context one render target's frame is drawn with.
	RenderCtx make_ctx(Output &output, wlr_render_pass *pass);
	// Everything from the canvas clear to the drag icon, into ctx's target.
	void draw_scene(const RenderCtx &ctx);
	// make_ctx() for an upright offscreen `target`: no capture UI, no cursors,
	// and a scratch surface list of its own that no frame feedback reads.
	RenderCtx make_capture_ctx(Output &output, wlr_render_pass *pass, wlr_buffer *target);
	void draw_lock(const RenderCtx &ctx);

	Server &server_;
	std::vector<DrawnSurface> drawn_; // surfaces drawn this frame, reused
	std::vector<DrawnSurface> capture_drawn_; // the same for captures, reused
};
