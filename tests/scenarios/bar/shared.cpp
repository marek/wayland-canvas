// Two outputs: both bars, each panel opened by its widget and by an action.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The walk's positions go stale as the bell widens, the agents widget comes and goes and the
// clock's text changes width: every indicator is found again from a fresh shot's runs. True
// once the shot showed all of them.
bool remeasure_indicators(std::vector<Run> runs) {
	static constexpr const char *right_to_left[] = {"power", "history",
		"brightness", "audio", "network", "bluetooth"};
	if (runs.size() < 2 + std::size(right_to_left)) {
		return false;
	}
	runs.erase(runs.begin()); // the logo
	runs.pop_back(); // the theme switch
	auto move = [](const char *name, double x) {
		for (Indicator &ind : indicators) {
			if (std::strcmp(ind.name, name) == 0) {
				ind.x = x;
			}
		}
	};
	for (std::size_t i = 0; i < std::size(right_to_left); i++) {
		move(right_to_left[i], runs[runs.size() - 1 - i].centre());
	}
	// The clock reads as two runs (the day, the time), the weather as one;
	// the gap between two widgets is wider than a space inside one.
	constexpr double widget_gap = 12;
	std::vector<Run> centred; // the clock, then the weather
	for (std::size_t i = 0; i + std::size(right_to_left) < runs.size(); i++) {
		if (std::abs(runs[i].centre() - screen_w / 2.0) > 250) {
			continue;
		}
		if (!centred.empty() && runs[i].x0 - centred.back().x1 <= widget_gap) {
			centred.back().x1 = runs[i].x1;
		} else {
			centred.push_back(runs[i]);
		}
	}
	if (centred.size() < 2) {
		return false;
	}
	move("clock", centred.front().centre());
	move("weather", centred.back().centre());
	return true;
}

// bar_surfaces_made before the second output joins: a bar made again for the
// first output would show as a second new one.
int bars_before_hotplug = 0;

// The notifications the bell's history keeps, from the plugin's own
// storage; -1 when there is no state directory to read.
int notification_history_length() {
	const char *state = std::getenv("XDG_STATE_HOME");
	if (state == nullptr) {
		return -1;
	}
	std::string path = std::format(
		"{}/wayland-canvas/plugins/canvas.notifications/notifications.json", state);
	JsonParser *parser = json_parser_new();
	int length = 0; // a history that was never written has no entries
	if (json_parser_load_from_file(parser, path.c_str(), nullptr)
			&& json_node_get_node_type(json_parser_get_root(parser)) == JSON_NODE_OBJECT) {
		JsonObject *stored = json_node_get_object(json_parser_get_root(parser));
		if (json_object_has_member(stored, "history")
				&& json_node_get_node_type(json_object_get_member(stored, "history"))
					== JSON_NODE_ARRAY) {
			length = int(json_array_get_length(
				json_node_get_array(json_object_get_member(stored, "history"))));
		}
	}
	g_object_unref(parser);
	return length;
}

// The notification plugin's popup surfaces on screen: one per output.
int notification_layers() {
	int n = 0;
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strcmp(l->wlr->namespace_,
							"wayland-canvas-notifications") == 0) {
					n++;
				}
			}
		}
	}
	return n;
}

int history_before = 0; // the history's length before the last notification

struct SharedPanel {
	const char *name;
	int width;
	int min_bands; // the least a card with its data shows
};
int handover_percent = 0;

constexpr SharedPanel panels[] = {
	{"network", network_panel, 10}, {"weather", weather_panel, 3},
	{"bluetooth", bluetooth_panel, 5}, {"power", power_panel, 10},
	{"brightness", brightness_panel, 5}, {"clock", clock_panel, 8},
	{"agents", agents_panel, 10},
};
size_t shared_panel = 0;

int shared_widget_bands = 0;

} // namespace

std::vector<harness::Step> shared_steps() {
	return {
		{"shared_second_output_was_added_for", [] {
			weather_fetches = weather_fetch_count();
			bars_before_hotplug = bar_surfaces_made;
			check(add_headless_output(), "a second output was added for the handover");
			return after(800);
		}},
		{"shared_second_output_added_for_bars", [] {
			if (auto wait = wait_for(server->outputs.size() >= 2, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			return after(100);
		}},
		{"shared_both_bars_are_up_each", [] {
			if (auto wait = wait_for(bar_layer_count() >= 2, {.poll_ms = 200, .budget_ms = 30000})) {
				return *wait;
			}
			check(bar_layer_count() == 2, "the second output got a bar of its own");
			// Adding an output makes that output's bar and nothing else: the
			// first bar, its widgets and what they hold, is the same surface.
			check(bar_surfaces_made == bars_before_hotplug + 1, std::format("adding an "
				"output made one bar, the first output's kept ({} bar surfaces made, "
				"{} before)", bar_surfaces_made, bars_before_hotplug));
			return after(1500); // its widgets are made and have asked for what they show
		}},
		{"shared_weather_is", [] {
			check(weather_fetch_count() == weather_fetches, std::format("the weather is "
				"fetched once with two bars ({} fetches, {} before the second output)",
				weather_fetch_count(), weather_fetches));
			return after(0);
		}},
		{"shared_bar_shot_checked", [] {
			// the indicators have moved since the walk measured them
			if (auto wait = shot_holds("shared-bar", [](cairo_surface_t *s) {
					LayerSurface *bar = bar_layer();
					return bar != nullptr && remeasure_indicators(bar_runs(s, bar->geo));
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("shared-bar");
			LayerSurface *layer = bar_layer();
			remeasure_indicators(s != nullptr && layer != nullptr
				? bar_runs(s, layer->geo) : std::vector<Run>{});
			cairo_surface_destroy(s);
			handover_percent = backlight_percent();
			wheel_over_bar(indicator_x("brightness"), 120);
			return after(500);
		}},
		{"shared_first_bars_wheel_stepped_the", [] {
			if (auto wait = wait_for(backlight_percent() == handover_percent + 5, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(backlight_percent() == handover_percent + 5, std::format("a wheel on the "
				"first bar's brightness stepped the backlight ({} %, was {} %)",
				backlight_percent(), handover_percent));
			wlr_box second{};
			wlr_output_layout_get_box(server->output_layout, server->outputs.back()->wlr, &second);
			wheel_over_bar(second.x + indicator_x("brightness"), 120);
			return after(500);
		}},
		{"shared_one_backlight_value_not_one", [] {
			if (auto wait = wait_for(backlight_percent() == handover_percent + 10, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(backlight_percent() == handover_percent + 10, std::format("a wheel on "
				"the second bar's brightness went on from the first bar's step ({} %, "
				"was {} %)", backlight_percent(), handover_percent));
			shared_panel = 0;
			return after(100);
		}},
		{"shared_first_panel_of_the_plugin", [] {
			run_msg("action.run", R"({"name":"canvas.scratch/open","args":{"panel":"canvas.halfway/one"}})",
				"open-halfway");
			return after(900);
		}},
		{"shared_halfway_panel_absent", [] {
			check(panel_layer("one") == nullptr, "a plugin whose second panel failed is "
				"skipped whole: its first panel is not there to open");
			return after(100);
		}},
		{"shared_widget_click", [] {
			const SharedPanel &panel = panels[shared_panel];
			double x = indicator_x(panel.name); // 0 for a widget the bar does not show

			// the widget's click opens the panel
			if (x == 0) {
				return go_to("shared_action_open", 50);
			}
			click(x, bar_mid_y());
			return after(700);
		}},
		{"shared_widget_open", [] {
			const SharedPanel &panel = panels[shared_panel];
			if (auto wait = wait_for(panel_layer(panel.name) != nullptr, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer(panel.name) != nullptr,
				std::format("the {} widget opened its panel", panel.name));
			return after(0);
		}},
		{"shared_widget_read", [] {
			const SharedPanel &panel = panels[shared_panel];
			double x = indicator_x(panel.name); // 0 for a widget the bar does not show

			// what the card shows, then close it
			std::string shot_name = std::format("shared-{}-widget", panel.name);
			auto card_bands = [&](cairo_surface_t *s) {
				return int(card_rows(s, card_box(x, panel.width)).size());
			};
			if (auto wait = shot_holds(shot_name.c_str(), [&](cairo_surface_t *s) {
					return card_bands(s) >= panel.min_bands;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png(shot_name.c_str());
			shared_widget_bands = s != nullptr ? card_bands(s) : 0;
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			std::println("  the {} widget's card: {} text bands", panel.name,
				shared_widget_bands);
			click_outside_cards(1100.0); // outside the card: the panel closes
			return after(600);
		}},
		{"shared_action_open", [] {
			const SharedPanel &panel = panels[shared_panel];
			// the action opens the same panel, with no anchor
			if (auto wait = wait_for(panel_layers() == 0, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			if (std::strcmp(panel.name, "agents") == 0) {
				// No widget shows them in the quiet world: the panel's own
				// open asks the collector for what it shows.
				drop_usage_fixtures();
				agents_runs_before = codex_runs();
			}
			run_msg("action.run", std::format(R"({{"name":"canvas.scratch/open",)"
				R"("args":{{"panel":"canvas.{0}/{0}"}}}})", panel.name),
				std::format("open-{}", panel.name).c_str());
			return after(700);
		}},
		{"shared_action_wait", [] {
			const SharedPanel &panel = panels[shared_panel];
			bool opened = panel_layer(panel.name) != nullptr
				&& !(std::strcmp(panel.name, "agents") == 0 && codex_runs() == agents_runs_before);
			if (auto wait = wait_for(opened, {.poll_ms = 250, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer(panel.name) != nullptr, std::format("an action opened "
				"the {} panel through api.panels.open", panel.name));
			return after(0);
		}},
		{"shared_action_read", [] {
			const SharedPanel &panel = panels[shared_panel];
			double x = indicator_x(panel.name); // 0 for a widget the bar does not show

			// the widget's card, at the left edge
			std::string shot_name = std::format("shared-{}-action", panel.name);
			auto card_bands = [&](cairo_surface_t *s) {
				return int(card_rows(s, wlr_box{8, bar_height(), panel.width, 0}).size());
			};
			if (auto wait = shot_holds(shot_name.c_str(), [&](cairo_surface_t *s) {
					return card_bands(s) >= panel.min_bands
						&& (x == 0 || card_bands(s) == shared_widget_bands);
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png(shot_name.c_str());
			int bands = s != nullptr ? card_bands(s) : 0;
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			std::println("  the {} panel an action opened: {} text bands", panel.name, bands);
			if (x != 0) {
				check(bands == shared_widget_bands, std::format("the {} panel an action "
					"opened shows what its widget's does ({} text bands, {} from the "
					"widget)", panel.name, bands, shared_widget_bands));
			}
			check(bands >= panel.min_bands, std::format("the {} panel an action opened "
				"shows its data, not an empty card ({} text bands, at least {} "
				"expected)", panel.name, bands, panel.min_bands));
			click_outside_cards(1100.0); // outside the card: the panel closes
			shared_panel++;
			return go_to("shared_next_panel", 600);
		}},
		{"shared_next_panel", [] {
			if (shared_panel < std::size(panels)) {
				return go_to("shared_widget_click");
			}
			// The notification's own handover, with both bars made. Two
			// servers would each take the notification: with the second
			// output's popups up, one notification must make one history entry.
			history_before = notification_history_length();
			notify_send("Handover A");
			return after(1200);
		}},
		{"shared_notification_landed", [] {
			int layers = notification_layers();
			if (auto wait = wait_for(notification_history_length() > history_before, {.poll_ms = 250, .budget_ms = 15000})) {
				return *wait;
			}
			check(layers == 2, std::format("the notification is drawn on both outputs "
				"({} popup surfaces)", layers));
			return after(1000); // a second server's entry would land in the same moment: let it
		}},
		{"shared_with_two_outputs_one", [] {
			int length = notification_history_length();
			check(length == history_before + 1, std::format("with two outputs, one "
				"notification made one history entry ({} entries, was {})",
				length, history_before));
			run_msg("action.run", R"({"name":"canvas.notifications/dismiss-all"})",
				"popups-dismiss-handover");
			return after(800);
		}},
	};
}

} // namespace bar
