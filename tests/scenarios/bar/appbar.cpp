// The app bar in the overview: its strip, its entries, a flyout of an app's windows.
#include "scenarios/bar/world.h"

namespace bar {

std::vector<harness::Step> appbar_steps() {
	return {
		themed_window_step("appbar_themed_window"),
		{"appbar_both_strips_on_one_edge", [] {
			// both strips on the right edge, so they make one
			// (docs/intents/shell/app-bar.md)
			check(set_toolbar_position("right") && set_app_bar_position("right"),
				"the toolbar and the app bar put on one edge in shell.json");
			return after(900); // the reload the edits trigger takes its time
		}},
		{"appbar_into_the_overview_where_both", [] {
			chord(KEY_TAB, mods::super);
			return after(600);
		}},
		{"appbar_one_strip", [] {
			LayerSurface *toolbar = toolbar_layer();
			LayerSurface *apps = app_bar_layer();
			bool ready = overview_settled(true) && toolbar != nullptr && apps != nullptr
				&& toolbar_on_edge("right");
			if (auto wait = wait_for(ready, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
			wlr_box app_strip = apps != nullptr ? app_bar_strip_box(apps->geo) : wlr_box{};
			check(toolbar != nullptr && apps != nullptr,
				"both strips mapped on the shared edge");
			check(app_strip.x == strip.x,
				std::format("the apps sit in the same column as the buttons"
					" ({} and {})", app_strip.x, strip.x));
			check(app_strip.y == strip.y + strip.height + strip_separator,
				std::format("the apps come last, a separator's gap after the"
					" buttons ({} after {}+{})", app_strip.y, strip.y, strip.height));
			check(toolbar_centred(strip), "the pair is centred along the edge as one");
			shot("app-bar-shared-edge");
			return after(500);
		}},
		{"appbar_click_on_an_apps_entry", [] {
			LayerSurface *apps = app_bar_layer();
			wlr_box app_strip = apps != nullptr ? app_bar_strip_box(apps->geo) : wlr_box{};
			app_bar_target = nullptr;
			for (const std::unique_ptr<View> &view : server->toplevels) {
				if (view->mapped) {
					app_bar_target = view.get(); // the first app's entry, the topmost row
					break;
				}
			}
			check(apps != nullptr && app_bar_target != nullptr,
				"an app bar entry to click, and the window it stands for");
			if (apps != nullptr) {
				app_bar_entry = {app_strip.x, app_strip.y, toolbar_button,
					toolbar_button};
				click(app_strip.x + toolbar_button / 2.0,
					app_strip.y + toolbar_button / 2.0);
			}
			return after(700);
		}},
		{"appbar_app_with_several_windows_unfolds", [] {
			int windows = app_bar_windows_of(app_bar_target);
			if (windows <= 1) {
				return after(100); // one window: the click already went there
			}
			LayerSurface *flyout = app_bar_flyout_layer();
			if (auto wait = wait_for(flyout != nullptr, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(flyout != nullptr, std::format("an app with {} windows unfolded "
				"them beside its entry", windows));
			auto [x, y] = app_bar_flyout_row(app_bar_entry, app_bar_target);
			click(x, y);
			return after(900);
		}},
		{"appbar_overview_left_the_view_on", [] {
			Output *output = server->outputs.front().get();
			bool landed = !output->overview.active() && !output->camera.panning()
				&& !output->camera.zooming() && app_bar_target != nullptr
				&& app_bar_target->frame_rect().intersects(output->viewport());
			if (auto wait = wait_for(landed, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			check(landed, "clicking an app left the overview and put its window "
				"on the screen");
			check(server->focus.focused_view() == app_bar_target,
				"and focused it");
			check(set_app_bar_enabled(false), "appbar.enabled: false written");
			return after(900);
		}},
		{"appbar_back_to_the_overview_with", [] {
			chord(KEY_TAB, mods::super);
			return after(600);
		}},
		{"appbar_toolbar_alone_centred_again", [] {
			LayerSurface *toolbar = toolbar_layer();
			bool ready = overview_settled(true) && toolbar != nullptr
				&& app_bar_layer() == nullptr;
			if (auto wait = wait_for(ready, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
			check(app_bar_layer() == nullptr,
				"switched off, the app bar shows on no screen");
			check(toolbar != nullptr && toolbar_centred(strip),
				"the toolbar is centred on its own again");
			return after(0);
		}},
	};
}

} // namespace bar
