// The app bar's hover labels.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// Nothing drawn in the band of `width` px just outside `box`'s right edge:
// what a component draws stays inside it, so a long title is elided rather
// than run past the card (the same read works for a strip or a chip).
bool nothing_right_of(cairo_surface_t *s, const wlr_box &box, int width) {
	if (s == nullptr) {
		return false;
	}
	int x = box.x + box.width + 1;
	int span = std::min(width, screen_w - x);
	if (span <= 0) {
		return true;
	}
	return bar_runs(s, {x, box.y, span, box.height}).empty();
}

} // namespace

std::vector<harness::Step> appbar_hover_steps() {
	return {
		themed_window_step("appbar_hover_themed_window"),
		app_windows_step("appbar_hover_several_windows_of_one_app", 2),
		overview_open_step("appbar_hover_overview_open"),
		{"appbar_hover_strip_back_on_the_left", [] {
			LayerSurface *apps = app_bar_layer();
			if (auto wait = wait_for(!((apps == nullptr || !overview_settled(true))), {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			wlr_box strip = apps != nullptr ? app_bar_strip_box(apps->geo) : wlr_box{};
			check(apps != nullptr && strip.x == toolbar_margin,
				std::format("the app bar is back on the left edge (strip {},{} {}x{})",
					strip.x, strip.y, strip.width, strip.height));
			move_to(strip.x + toolbar_button / 2.0, strip.y + toolbar_button / 2.0);
			return after(500);
		}},
		{"appbar_hover_hover_drawn", [] {
			shot("app-bar-hover");
			return after(600);
		}},
		{"appbar_hover_hovered_apps_name_on_a", [] {
			LayerSurface *apps = app_bar_layer();
			wlr_box strip = apps != nullptr ? app_bar_strip_box(apps->geo) : wlr_box{};
			cairo_surface_t *s = load_png("app-bar-hover");
			// The chip sits one gap right of the strip, level with the entry.
			std::vector<Run> runs = s != nullptr ? bar_runs(s,
				{strip.x + toolbar_button, strip.y, 240, toolbar_button})
				: std::vector<Run>{};
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			if (auto wait = wait_for(!runs.empty(), {.poll_ms = 600, .budget_ms = 15000})) {
				shot("app-bar-hover");
				return *wait;
			}
			check(!runs.empty(), std::format("hovering an entry shows the app's "
				"name beside it, toward the screen's centre ({} runs)", runs.size()));
			app_bar_target = first_mapped_toplevel();
			app_bar_entry = {strip.x, strip.y, toolbar_button, toolbar_button};
			click(strip.x + toolbar_button / 2.0, strip.y + toolbar_button / 2.0);
			return after(700);
		}},
		{"appbar_hover_several_windows", [] {
			if (app_bar_windows_of(app_bar_target) <= 1) {
				return after(300); // one window: the click already went there
			}
			LayerSurface *flyout = app_bar_flyout_layer();
			if (auto wait = wait_for(flyout != nullptr, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(flyout != nullptr,
				"the left-edge entry unfolded its windows beside it");
			shot("app-bar-flyout");
			return after(700);
		}},
		{"appbar_hover_nothing_of_the_rows_is", [] {
			auto [x, y] = app_bar_flyout_row(app_bar_entry, app_bar_target);
			wlr_box card{int(x - 120), app_bar_entry.y, 240,
				std::min(settings_row, screen_h - app_bar_entry.y)};
			cairo_surface_t *s = load_png("app-bar-flyout");
			bool inside = nothing_right_of(s, card, 80);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			if (auto wait = wait_for(inside, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("app-bar-flyout");
				return *wait;
			}
			check(inside, "a window's title is elided inside the card: nothing of "
				"it is drawn past its right edge");
			click(x, y);
			return after(900);
		}},
		{"appbar_hover_click_took_the_view_to", [] {
			Output *output = server->outputs.front().get();
			bool landed = !output->overview.active() && !output->camera.panning()
				&& !output->camera.zooming() && app_bar_target != nullptr
				&& app_bar_target->frame_rect().intersects(output->viewport());
			if (auto wait = wait_for(landed, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			check(landed, "a click on the left-edge app bar took the view to the "
				"window");
			return after(0);
		}},
	};
}

} // namespace bar
