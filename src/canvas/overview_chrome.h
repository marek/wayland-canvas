#pragma once

#include <cstddef>
#include <optional>
// libstdc++ 16's <inplace_vector> uses the C++26-deprecated is_trivial_v internally.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <inplace_vector>
#pragma clang diagnostic pop
#include "core/wlr.h"

class Desktop;
class Output;
class Server;
class View;
struct CachedText;
struct GestureOptions;
struct RenderCtx;

// What the overview shows besides the canvas: a tab above each desktop frame,
// the shelf of minimized windows, and the help line. Geometry is output-local
// logical coordinates; what is drawn and what takes the pointer read the same
// boxes.

// The tab above a desktop frame and its Tile and Abandon buttons.
struct TabLayout {
	wlr_box tab{}, tile_button{}, abandon_button{};
};

// nullopt when the tab is off-screen.
std::optional<TabLayout> overview_tab_layout(const Output &output, const Desktop &desktop);

inline constexpr std::size_t shelf_max = 64;

struct ShelfItem {
	View *view = nullptr;
	wlr_box box{}; // thumbnail
};
using Shelf = std::inplace_vector<ShelfItem, shelf_max>;

// The overview's bottom stack on an output, from its bottom edge (or the top of
// the highest strip anchored to it) up: the help line, the row a hint takes
// above it, and the shelf's bottom above both.
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

// The overview's help line for the gesture options in force: the swipe
// preset's gestures with the pointer and key hints, or only the hints when no
// preset binds any.
const char *overview_help_text(const GestureOptions &options);

// A desktop tab's label at `scale`, from the text cache; `on_accent` is the
// target tab's variant, in the contrasting accent text colour instead of the
// panel text colour.
CachedText desktop_label(Server &server, const Desktop &desktop, float scale, bool on_accent);

// Draws the tabs, the shelf and the help line of `ctx`'s output over the
// canvas; `bottom` is overview_bottom_layout() of that output.
void draw_overview_chrome(const RenderCtx &ctx, const OverviewBottom &bottom);
