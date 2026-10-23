// The toolbar's four edges, and the note flyout on each.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The note menu's card, the theme's metrics (shell/ui/.../UiTheme.qml):
// six rows of 32 px with 6 px between them, in 10 px padding; the 1 px
// border hairline draws inside, so this is the card's height.
constexpr int note_menu_rows = 6;

constexpr int note_menu_height = note_menu_rows * 32
	+ (note_menu_rows - 1) * 6 + 2 * 10;

// The card hangs from the button's top (the flyout anchors itself there): its rows are read
// from a region that starts a margin above the button's band, with a margin's slack on
// either side.
constexpr int note_menu_margin = 16;

// "#rrggbb" to its channels; false when it is not that shape.
bool hex_colour(std::string_view text, unsigned char out[3]) {
	auto hex = [](char ch) -> int {
		if (ch >= '0' && ch <= '9') {
			return ch - '0';
		}
		if (ch >= 'a' && ch <= 'f') {
			return ch - 'a' + 10;
		}
		return -1;
	};
	if (text.size() != 7 || text.front() != '#') {
		return false;
	}
	for (int channel = 0; channel < 3; channel++) {
		int hi = hex(text[1 + channel * 2]);
		int lo = hex(text[2 + channel * 2]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out[channel] = static_cast<unsigned char>(hi * 16 + lo);
	}
	return true;
}

// The overview's bottom stack on the first output, as the compositor draws
// and hit-tests it.
OverviewBottom overview_stack() {
	Output &output = *server->outputs.front();
	return overview_bottom_layout(output, output.wlr->scale);
}

// Whether the minimized window's shelf thumbnails end where the stack puts
// them, above the help line.
bool shelf_above_help(const Shelf &shelf, const OverviewBottom &stack) {
	if (shelf.empty()) {
		return false;
	}
	int bottom = shelf.front().box.y + shelf.front().box.height;
	return bottom == stack.shelf_bottom && bottom < stack.help.y;
}

// The note button's place in the strip: order 3, the fourth square —
// the scratch plugin's three buttons above it, the settings gear
// (order 4) below. Keep in sync with the plugins' manifests.
constexpr int notes_index = 3;

bool app_bar_on_edge(const char *edge) {
	return strip_on_edge(app_bar_layer(), edge);
}

// How many apps the compositor has open: the app bar draws one entry each,
// grouped by app id (a window without one stands alone).
int apps_open() {
	std::vector<std::string> seen;
	for (const std::unique_ptr<View> &view : server->toplevels) {
		if (!view->mapped) {
			continue;
		}
		const char *app_id = view->shell_app_id();
		std::string key = app_id != nullptr && *app_id != '\0'
			? std::string{app_id}
			: std::format("window:{}", view->session_id);
		if (std::ranges::find(seen, key) == seen.end()) {
			seen.push_back(key);
		}
	}
	return static_cast<int>(seen.size());
}

// The notes button's click point, from the placed surface.
std::pair<double, double> notes_click(const wlr_box &geo) {
	wlr_box strip = toolbar_strip_box(geo);
	int along = notes_index * toolbar_button + toolbar_button / 2;
	if (strip.width == toolbar_button) {
		return {strip.x + toolbar_button / 2.0, double(strip.y + along)};
	}
	return {double(strip.x + along), strip.y + toolbar_button / 2.0};
}

// The generous region the notes flyout opens into: a 200 px card of the six-row menu's
// height, one margin past the strip on its inner side, its near edge at the button's near
// one, with slack on every side for the clamping and the rounding.
wlr_box flyout_region(const wlr_box &geo) {
	wlr_box strip = toolbar_strip_box(geo);
	int near = notes_index * toolbar_button; // the button's near edge
	int x = 0, y = 0;
	if (strip.width == toolbar_button) { // a column: the card beside it
		x = strip.x > screen_w / 2 ? strip.x - beside_gap - scratch_card
			: strip.x + toolbar_button + beside_gap;
		y = strip.y + near;
	} else { // a row: the card above or below it
		y = strip.y > screen_h / 2 ? strip.y - beside_gap - note_menu_height
			: strip.y + toolbar_button + beside_gap;
		x = strip.x + near;
	}
	return {x - note_menu_margin, y - note_menu_margin,
		scratch_card + 2 * note_menu_margin,
		note_menu_height + 2 * note_menu_margin};
}

// The note menu's yellow swatch as pixel_is takes it: 0xRRGGBBAA.
uint32_t note_swatch_colour() {
	unsigned char rgb[3];
	hex_colour(note_yellow, rgb);
	return uint32_t(rgb[0]) << 24 | uint32_t(rgb[1]) << 16
		| uint32_t(rgb[2]) << 8 | 0xff;
}

// The checks every edge shares, once the strip has arrived on it: the
// placed box and the centring along the edge, then the shot. The shelf
// checks differ — only a bottom toolbar moves it.
void edge_checks(const char *edge, const char *shot_name) {
	LayerSurface *toolbar = toolbar_layer();
	wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
	const char *place = std::strcmp(edge, "left") == 0
		? "8 px from the left edge"
		: std::strcmp(edge, "top") == 0 ? "below the bar, 8 px down"
		: std::strcmp(edge, "bottom") == 0 ? "8 px above the bottom edge"
		: "8 px from the right edge";
	check(toolbar != nullptr && toolbar_on_edge(edge),
		std::format("the strip moved to {} (strip {},{} {}x{})", place,
			toolbar != nullptr ? strip.x : -1, toolbar != nullptr ? strip.y : -1,
			toolbar != nullptr ? strip.width : 0, toolbar != nullptr ? strip.height : 0));
	check(toolbar != nullptr && toolbar_centred(strip),
		"the strip is centred along its edge");
	// The app bar on this edge too: the two are one strip, the buttons
	// first, the apps a separator's gap after them.
	LayerSurface *apps = app_bar_layer();
	wlr_box app_strip = apps != nullptr ? app_bar_strip_box(apps->geo) : wlr_box{};
	bool column = strip.width == toolbar_button;
	int gap = column ? app_strip.y - (strip.y + strip.height)
		: app_strip.x - (strip.x + strip.width);
	check(apps != nullptr && app_bar_on_edge(edge) && gap == strip_separator,
		std::format("the app bar shares the {} edge, a separator's gap after the"
			" buttons (gap {})", edge, gap));
	shot(shot_name);
}

// Clicks the notes button (its square, from the placed strip), then
// parks the pointer clear of every edge's strip and card.
void open_notes_flyout() {
	LayerSurface *toolbar = toolbar_layer();
	if (toolbar != nullptr) {
		auto [x, y] = notes_click(toolbar->geo);
		click(x, y);
	}
	move_to(640, 360);
}

int flyout_retries = 0;

// The open-flyout checks on a landed shot: the yellow swatch row, and the rows with it.
// False while the click that opens it may have been lost under load: the caller retries on
// the next tick and this re-clicks on every third.
bool flyout_open_checks(const char *shot_name, const char *toward) {
	cairo_surface_t *s = load_png(shot_name);
	LayerSurface *toolbar = toolbar_layer();
	wlr_box region = toolbar != nullptr ? flyout_region(toolbar->geo) : wlr_box{};
	int swatch = colour_count(s, region, note_swatch_colour());
	std::vector<Band> rows = bands_in(s, region);
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	if (swatch <= 20 && flyout_retries++ < 40) {
		if (flyout_retries % 3 == 1) {
			open_notes_flyout();
		}
		return false;
	}
	flyout_retries = 0;
	check(swatch > 20, std::format("the note flyout opened {} the strip, "
		"toward the screen's centre ({} yellow swatch pixels)", toward, swatch));
	check(rows.size() >= 2, std::format("the flyout's rows with it "
		"({} bands)", rows.size()));
	return true;
}

// The folded checks on a landed shot: the swatch gone. False while the
// second click — the fold — may have been lost.
bool flyout_folded_checks(const char *shot_name) {
	cairo_surface_t *s = load_png(shot_name);
	LayerSurface *toolbar = toolbar_layer();
	wlr_box region = toolbar != nullptr ? flyout_region(toolbar->geo) : wlr_box{};
	int swatch = colour_count(s, region, note_swatch_colour());
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	if (swatch > 20 && flyout_retries++ < 40) {
		if (flyout_retries % 3 == 1) {
			open_notes_flyout(); // the button again: the toggle folds
		}
		return false;
	}
	flyout_retries = 0;
	check(swatch <= 20, "the flyout folded on the button's second click");
	return true;
}

} // namespace

std::vector<harness::Step> edges_steps() {
	return {
		themed_window_step("edges_themed_window"),
		{"edges_themed_window_focused_for_the", [] {
			View *v = titled("themed");
			wlr_box f = v != nullptr ? v->frame_box() : wlr_box{};
			if (v != nullptr && server->focus.focused_view() != v) {
				click(f.x + f.width / 2.0, f.y + f.height / 2.0);
			}
			if (auto wait = wait_for(server->focus.focused_view() == v, {.poll_ms = 400, .budget_ms = 15000})) {
				return *wait;
			}
			check(server->focus.focused_view() == v, "the themed window is focused");
			chord(KEY_M, mods::super); // Super+M minimizes it
			return after(400);
		}},
		{"edges_minimized", [] {
			View *v = titled("themed");
			if (auto wait = wait_for(!(v != nullptr && !v->minimized), {.poll_ms = 400, .budget_ms = 15000})) {
				if (polls() % 5 == 2) {
					chord(KEY_M, mods::super); // a chord may be lost: send it again
				}
				return *wait;
			}
			check(v != nullptr && v->minimized,
				"Super+M minimized the themed window (the shelf's item)");
			chord(KEY_TAB, mods::super); // the overview: the toolbar's home
			return after(400);
		}},
		{"edges_toolbar_on_its_default_right", [] {
			if (auto wait = wait_for(overview_settled(true) && toolbar_settled_on("right"), {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			LayerSurface *toolbar = toolbar_layer();
			wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
			check(overview_settled(true), "the overview settled for the toolbar");
			check(toolbar != nullptr && toolbar_on_edge("right"),
				"the toolbar mapped 8 px from the right edge, its default");
			check(toolbar != nullptr && toolbar_centred(strip),
				"the strip is centred along its edge");
			// The notes and settings buttons and the scratch plugin's three
			// valid ones: the scratch manifest's two invalid buttons (one
			// naming an action and a panel, one naming nothing) were dropped
			// by the host, and the toolbar draws what it is given.
			check(toolbar != nullptr && strip.height / toolbar_button == 5,
				std::format("the strip holds exactly its five valid buttons ({} squares)",
					toolbar != nullptr ? strip.height / toolbar_button : 0));
			// The app bar: its default left edge, opposite the toolbar's, one
			// entry per open app (docs/intents/shell/app-bar.md).
			LayerSurface *apps = app_bar_layer();
			wlr_box app_strip = apps != nullptr ? app_bar_strip_box(apps->geo) : wlr_box{};
			check(apps != nullptr && app_strip.x == toolbar_margin
					&& app_strip.width == toolbar_button,
				std::format("the app bar mapped 8 px from the left edge, its default"
					" (strip {},{} {}x{})", app_strip.x, app_strip.y,
					app_strip.width, app_strip.height));
			check(apps != nullptr && app_strip.height / toolbar_button == apps_open(),
				std::format("the app bar holds one entry per open app ({} of {})",
					apps != nullptr ? app_strip.height / toolbar_button : 0, apps_open()));
			check(apps != nullptr
					&& app_strip.y + app_strip.height / 2 == screen_h / 2,
				"the app bar's strip is centred along its own edge");
			check(set_toolbar_position("left"),
				"toolbar.position: left written to the scratch shell.json");
			return after(800); // the reload the edit triggers takes its time
		}},
		{"edges_left_edge", [] {
			if (auto wait = wait_for(toolbar_settled_on("left"), {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			edge_checks("left", "edges-left");
			Shelf shelf = overview_shelf_layout(*server->outputs.front());
			OverviewBottom stack = overview_stack();
			check(!shelf.empty(), "the minimized window sits on the shelf");
			check(shelf_above_help(shelf, stack)
					&& stack.help.y + stack.help.height
						== screen_h - config::overview_help_margin,
				"the shelf keeps its place along the bottom (a side toolbar "
				"clears nothing)");
			return after(700);
		}},
		{"edges_note_button", [] {
			if (auto wait = wait_for(toolbar_layer() != nullptr, {.poll_ms = 400, .budget_ms = 15000})) {
				return *wait;
			}
			open_notes_flyout();
			return after(400);
		}},
		{"edges_flyout_open", [] {
			if (!shot_landed("edges-left-flyout")
					|| !flyout_open_checks("edges-left-flyout", "right of")) {
				return again(100);
			}
			open_notes_flyout(); // the button again: the toggle folds
			return after(400);
		}},
		{"edges_folded", [] {
			if (!shot_landed("edges-left-folded")
					|| !flyout_folded_checks("edges-left-folded")) {
				return again(100);
			}
			check(set_toolbar_position("top") && set_app_bar_position("top"),
				"toolbar.position and appbar.position: top written to the scratch shell.json");
			return after(800);
		}},
		{"edges_top_edge", [] {
			if (auto wait = wait_for(toolbar_settled_on("top") && app_bar_on_edge("top"), {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			edge_checks("top", "edges-top");
			return after(700);
		}},
		{"edges_note_button_2", [] {
			if (auto wait = wait_for(toolbar_layer() != nullptr, {.poll_ms = 400, .budget_ms = 15000})) {
				return *wait;
			}
			open_notes_flyout();
			return after(400);
		}},
		{"edges_flyout_open_2", [] {
			if (!shot_landed("edges-top-flyout")
					|| !flyout_open_checks("edges-top-flyout", "below")) {
				return again(100);
			}
			open_notes_flyout(); // the button again: the toggle folds
			return after(400);
		}},
		{"edges_folded_2", [] {
			if (!shot_landed("edges-top-folded")
					|| !flyout_folded_checks("edges-top-folded")) {
				return again(100);
			}
			check(set_toolbar_position("bottom") && set_app_bar_position("bottom"),
				"toolbar.position and appbar.position: bottom written to the scratch shell.json");
			return after(800);
		}},
		{"edges_bottom_edge", [] {
			if (auto wait = wait_for(toolbar_settled_on("bottom") && app_bar_on_edge("bottom"), {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			edge_checks("bottom", "edges-bottom");
			LayerSurface *toolbar = toolbar_layer();
			wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
			Shelf shelf = overview_shelf_layout(*server->outputs.front());
			OverviewBottom stack = overview_stack();
			check(!shelf.empty(), "the minimized window sits on the shelf");
			check(toolbar != nullptr && shelf_above_help(shelf, stack)
					&& stack.help.y + stack.help.height
						== strip.y - config::overview_help_margin,
				std::format("the shelf rose above the bottom toolbar (its bottom "
					"at {}, the help line's top at {}, the toolbar's top at {})",
					!shelf.empty() ? shelf.front().box.y + shelf.front().box.height : 0,
					stack.help.y, toolbar != nullptr ? strip.y : 0));
			return after(700);
		}},
		{"edges_note_button_3", [] {
			if (auto wait = wait_for(toolbar_layer() != nullptr, {.poll_ms = 400, .budget_ms = 15000})) {
				return *wait;
			}
			open_notes_flyout();
			return after(400);
		}},
		{"edges_flyout_open_3", [] {
			if (!shot_landed("edges-bottom-flyout")
					|| !flyout_open_checks("edges-bottom-flyout", "above")) {
				return again(100);
			}
			open_notes_flyout(); // the button again: the toggle folds
			return after(400);
		}},
		{"edges_folded_3", [] {
			if (!shot_landed("edges-bottom-folded")
					|| !flyout_folded_checks("edges-bottom-folded")) {
				return again(100);
			}
			check(set_toolbar_position("right") && set_app_bar_position("right"),
				"toolbar.position and appbar.position: right written to the scratch shell.json");
			return after(800);
		}},
		{"edges_right_edge_again_as_the", [] {
			if (auto wait = wait_for(toolbar_settled_on("right") && app_bar_on_edge("right"), {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			edge_checks("right", "edges-right");
			return after(700);
		}},
		{"edges_note_button_4", [] {
			if (auto wait = wait_for(toolbar_layer() != nullptr, {.poll_ms = 400, .budget_ms = 15000})) {
				return *wait;
			}
			open_notes_flyout();
			return after(400);
		}},
		{"edges_flyout_open_4", [] {
			if (!shot_landed("edges-right-flyout")
					|| !flyout_open_checks("edges-right-flyout", "left of")) {
				return again(100);
			}
			open_notes_flyout(); // the button again: the toggle folds
			return after(400);
		}},
		{"edges_folded_4", [] {
			if (!shot_landed("edges-right-folded")
					|| !flyout_folded_checks("edges-right-folded")) {
				return again(100);
			}
			check(toolbar_layer() != nullptr, "the toolbar stays after its edges");
			tap(KEY_ESC); // Escape leaves the overview, folding everything
			return after(400);
		}},
		{"edges_toolbars_edges_done", [] {
			if (auto wait = wait_for(!((!overview_settled(false) || toolbar_layer() != nullptr)), {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			check(overview_settled(false), "the overview closed behind the toolbar's edges");
			check(toolbar_layer() == nullptr, "the toolbar slid out with the overview");
			check(panel_layers() == 0, "no panel outlived the toolbar's edges");
			check(toolbar_position_in_config() == "right",
				"the scratch shell.json ends with toolbar.position: right");
			watch_remember_bar();
			check(set_app_bar_position("left"), "the app bar put back on its default edge");
			return after(800);
		}},
	};
}

} // namespace bar
