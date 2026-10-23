// A plugin's own page in the settings window, and the slider and notification settings.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The Plugins tab's column of content: what canvas.notifications declares —
// the Popups group (three rows) and the History group (one)
// (docs/intents/shell/plugins.md). Keep in sync with its manifest.
int settings_plugins_column() {
	return settings_section_height(0, 3)
		+ settings_section_height(0, 1)
		+ settings_gap;
}

// Every tab's label inside its own share, with a pixel of air each side: a
// label that outgrew its cell would run into its neighbour's, and a long
// name or one tab more must elide instead.
bool settings_tab_labels_within_cells(cairo_surface_t *s, int card_y) {
	for (int i = 0; i < settings_tabs; i++) {
		auto [cx, cy] = settings_tab_point(i, card_y);
		std::vector<Run> runs = bar_runs(s,
			{int(cx - settings_tab_w / 2.0), int(cy - settings_row / 2.0),
				settings_tab_w, settings_row});
		if (runs.empty()) {
			return false;
		}
		if (runs.front().x0 < 1 || runs.back().x1 > settings_tab_w - 1) {
			return false;
		}
	}
	return true;
}

// The popup cards a shot of the first output shows. An urgent card has a
// one-pixel attention border (Popups.qml), so the rows of that colour down
// a line inside the cards' left padding come two to a card. The line is
// the column's: the edge margin and the width Popups.qml gives it.
int popup_cards(cairo_surface_t *s) {
	if (s == nullptr) {
		return -1;
	}
	int x = screen_w - 8 - 340 + 40;
	int rows = 0;
	for (int y = bar_height() + 2; y < screen_h; y++) {
		if (pixel_is(s, x, y, attention_dark)) {
			rows++;
		}
	}
	return rows / 2;
}

// The writes shell.json gets, counted by an inotify watch on its directory:
// start before the action, read (and stop) after it.
int shell_json_watch = -1;

void shell_json_watch_start() {
	if (shell_json_watch >= 0) {
		close(shell_json_watch);
	}
	shell_json_watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	std::string dir = std::filesystem::path(shell_config).parent_path().string();
	inotify_add_watch(shell_json_watch, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
}

int shell_json_watch_writes() {
	int writes = 0;
	alignas(inotify_event) char buffer[4096];
	ssize_t len = 0;
	while (shell_json_watch >= 0 && (len = read(shell_json_watch, buffer, sizeof buffer)) > 0) {
		for (ssize_t at = 0; at < len;) {
			const auto *event = reinterpret_cast<const inotify_event *>(buffer + at);
			if (event->len > 0 && std::strcmp(event->name, "shell.json") == 0) {
				writes++;
			}
			at += ssize_t(sizeof(inotify_event) + event->len);
		}
	}
	if (shell_json_watch >= 0) {
		close(shell_json_watch);
		shell_json_watch = -1;
	}
	return writes;
}

// The Plugins page's number slider (PluginSettingRow.qml) and where its
// row lies: the n-th row of the first group, from the card's own metrics.
constexpr int plugin_slider_w = 120;
double slider_left = 0;

double slider_y = 0;

double plugins_row_y(int index) {
	int column = settings_plugins_column();
	int card_y = settings_card_top(settings_card_height(column), settings_gear_y);
	int first_top = card_y + settings_pad + settings_tabs_h + settings_gap
		+ settings_header + settings_gap;
	return first_top + index * (settings_choice + settings_gap) + settings_choice / 2.0;
}

// One plugin setting the scratch shell.json holds, as text ("true", "42",
// a string's own value); empty when the config names none.
std::string plugin_setting_text(const char *id, const char *key) {
	JsonParser *parser = json_parser_new();
	std::string out;
	if (json_parser_load_from_file(parser, shell_config.c_str(), nullptr)
			&& json_node_get_node_type(json_parser_get_root(parser))
				== JSON_NODE_OBJECT) {
		JsonObject *at = json_node_get_object(json_parser_get_root(parser));
		for (const char *step : {"plugins", id, "settings"}) {
			if (at == nullptr || !json_object_has_member(at, step)
					|| json_node_get_node_type(json_object_get_member(at, step))
						!= JSON_NODE_OBJECT) {
				at = nullptr;
				break;
			}
			at = json_node_get_object(json_object_get_member(at, step));
		}
		if (at != nullptr && json_object_has_member(at, key)) {
			JsonNode *node = json_object_get_member(at, key);
			if (json_node_get_node_type(node) == JSON_NODE_VALUE) {
				GType type = json_node_get_value_type(node);
				if (type == G_TYPE_BOOLEAN) {
					out = json_node_get_boolean(node) ? "true" : "false";
				} else if (type == G_TYPE_INT64) {
					out = std::format("{}", json_node_get_int(node));
				} else if (type == G_TYPE_STRING) {
					out = json_node_get_string(node);
				}
			}
		}
	}
	g_object_unref(parser);
	return out;
}

} // namespace

std::vector<harness::Step> plugin_settings_steps() {
	return {
		overview_open_step("plugin_settings_overview_open"),
		{"plugin_settings_gear_clicked", [] {
			// On to the Plugins page: the settings window renders what a plugin
			// declares, no plugin named in its own code
			// (docs/intents/shell/plugins.md).
			if (settings_gear_y == 0) {
				settings_gear_y = toolbar_gear_centre();
			}
			settings_card_x = settings_card_left(false);
			click(strip_left + toolbar_button / 2.0, settings_gear_y);
			return after(600);
		}},
		{"plugin_settings_panel_open_again", [] {
			if (auto wait = wait_for(panel_layer("settings") != nullptr, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("settings") != nullptr,
				"the gear opened the settings panel for the Plugins page");
			{
				auto [tx, ty] = settings_tab_point(5, 8); // Plugins
				click(tx, ty);
			}
			return after(700);
		}},
		{"plugin_settings_page_redraws", [] {
			shot("settings-plugins-tab");
			return after(700);
		}},
		{"plugin_settings_declared_rows_drawn", [] {
			cairo_surface_t *s = load_png("settings-plugins-tab");
			int column = settings_plugins_column();
			int card_y = settings_card_top(settings_card_height(column), settings_gear_y);
			int first_top = card_y + settings_pad + settings_tabs_h + settings_gap
				+ settings_header + settings_gap;
			// The second row of the Popups group: the urgent-popups switch.
			double switch_y = first_top + settings_choice + settings_gap
				+ settings_choice / 2.0;
			std::vector<Band> rows = bands_in(s,
				{settings_card_x + 8, card_y, settings_label_w,
					settings_card_height(column)});
			int chosen = s != nullptr ? settings_chosen_tab(s, card_y) : -1;
			bool on = s != nullptr && settings_switch_is(s, switch_y, true);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			auto row = settings_band_at(rows, switch_y);
			bool labelled = row != rows.end()
				&& std::abs(row->centre() - switch_y) <= 12;
			if (auto wait = wait_for(!((chosen != 5 || !labelled || !on)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-plugins-tab");
				return *wait;
			}
			check(chosen == 5, "the Plugins tab is the one chosen");
			{
				cairo_surface_t *shot_s = load_png("settings-plugins-tab");
				bool inside = settings_tab_labels_within_cells(shot_s, card_y);
				if (shot_s != nullptr) {
					cairo_surface_destroy(shot_s);
				}
				check(inside, std::format("every tab label stays inside its own "
					"share of the row ({} tabs)", settings_tabs));
			}
			check(labelled, "the notification plugin's declared switch has its row");
			check(on, "the switch shows the declared default, on");
			click(settings_card_x + settings_panel - settings_pad - 8
					- settings_switch_w / 2.0, switch_y);
			return after(500);
		}},
		{"plugin_settings_click_wrote_the_plugins_own", [] {
			std::string held = plugin_setting_text("canvas.notifications",
				"criticalNeverExpires");
			if (auto wait = wait_for(held == "false", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(held == "false", std::format("the switch wrote"
				" plugins.canvas.notifications.settings.criticalNeverExpires"
				" into shell.json ({})", held.empty() ? "absent" : held));
			// The Popups group's third row is a number, "Popups at once": one
			// drag of its slider is one write of shell.json, and the next burst
			// of notifications shows the new number.
			return after(300);
		}},
		{"plugin_settings_panel_still_open_on_the", [] {
			slider_left = settings_card_x + settings_panel - settings_pad - 8 - plugin_slider_w;
			slider_y = plugins_row_y(2);
			// The knob rests a third along the track (4 of 1 to 10): the fill
			// runs from the track's left to it. Its rows give the slider's own
			// centre, whatever the page's metrics.
			auto fill_rows = [&](cairo_surface_t *s) {
				std::pair<int, int> rows{-1, -1};
				for (int y = int(slider_y) - 12; s != nullptr && y <= int(slider_y) + 12; y++) {
					if (pixel_is(s, int(slider_left) + 10, y, accent_dark)) {
						if (rows.first < 0) {
							rows.first = y;
						}
						rows.second = y;
					}
				}
				return rows;
			};
			if (auto wait = shot_holds("settings-plugins-tab", [&](cairo_surface_t *s) {
					return fill_rows(s).first >= 0;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("settings-plugins-tab");
			auto [top, bottom] = fill_rows(s);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(top >= 0, std::format("the Popups-at-once slider sits where the page lays "
				"it (its fill found at {}-{}, the row centred at {})", top, bottom, slider_y));
			if (top >= 0) {
				slider_y = (top + bottom + 1) / 2.0;
			}
			shell_json_watch_start();
			move_to(slider_left + 40, slider_y); // onto the knob
			return after(400);
		}},
		{"plugin_settings_press_stays_held_while_the", [] {
			button(BTN_LEFT, true);
			return after(400);
		}},
		{"plugin_settings_knob_dragged_left_to_the", [] {
			travel(slider_left + 40, slider_y, slider_left + 14, slider_y);
			return after(400);
		}},
		{"plugin_settings_knob_released", [] {
			button(BTN_LEFT, false);
			return after(800);
		}},
		{"plugin_settings_one_drag_one_write", [] {
			std::string held = plugin_setting_text("canvas.notifications", "maxPopups");
			if (auto wait = wait_for(held == "2", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			int writes = shell_json_watch_writes();
			check(held == "2", std::format("the drag wrote plugins.canvas.notifications"
				".settings.maxPopups: 2 into shell.json ({})", held.empty() ? "absent" : held));
			check(writes == 1, std::format("one drag of a number setting wrote shell.json "
				"once ({} writes)", writes));
			tap(KEY_ESC); // the settings panel closes; the overview stays
			return after(600);
		}},
		{"plugin_settings_burst_of_four_notifications", [] {
			for (const char *summary : {"Burst one", "Burst two", "Burst three", "Burst four"}) {
				notify_send(summary);
			}
			return after(1500);
		}},
		{"plugin_settings_shot_popups_burst", [] {
			shot("popups-burst");
			return after(700);
		}},
		{"plugin_settings_burst_shows_the_two_popups", [] {
			cairo_surface_t *s = load_png("popups-burst");
			int cards = popup_cards(s);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// the cards may still be arriving
			if (auto wait = wait_for(cards == 2, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("popups-burst");
				return *wait;
			}
			check(cards == 2, std::format("the burst shows the two popups the setting now "
				"allows ({} cards)", cards));
			run_msg("action.run", R"({"name":"canvas.notifications/dismiss-all"})",
				"popups-dismiss-all");
			return after(800);
		}},
		{"plugin_settings_cards_gone", [] {
			// Back to the app bar, on its own default edge this time: the hover
			// label and the jump are the two things a user touches
			// (docs/intents/shell/app-bar.md).
			check(set_app_bar_enabled(true) && set_app_bar_position("left"),
				"the app bar switched back on, on its default left edge");
			return after(900);
		}},
	};
}

} // namespace bar
