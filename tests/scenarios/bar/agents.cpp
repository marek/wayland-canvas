// The agents panel end to end.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

std::size_t agents_base_runs = 0;
int usage_requests_base = 0; // the stand-in's log lines before the reopen
std::string agents_config_kept; // shell.json before the refresh setting went in

long long agents_first_run_ms = 0; // when the first run after the reload showed

wlr_box agents_box{}; // the panel's card on screen

// The provider chips of the open agents panel, as runs in its hero row.
std::vector<Run> agents_chip_runs;

// One JSON object member as a whole number, or 0 when it is absent.
long long json_int(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return 0;
	}
	JsonNode *node = json_object_get_member(object, name);
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_INT64
		? json_node_get_int(node) : 0;
}

constexpr int UiThemePadding = 10; // UiTheme.panelPadding, mirrored here

constexpr uint32_t claude_mark = 0xd97757ff;   // the starburst's clay
constexpr uint32_t muted_dark = 0x565f89ff;    // UiTheme.muted, dark

// A selected chip's body in the agents panel's switch: the accent at 22 %
// over UiTheme.panel, dark.
constexpr uint32_t chip_selected_dark = 0x2f3954ff;
constexpr uint32_t muted_light = 0x8489a3ff;   // UiTheme.muted, light
constexpr uint32_t panel_light = 0xf4f5f8ff;   // UiTheme.panel, light

// The collector ranks the families after Claude and GPT by the week's
// tokens: a day's work through the gateway puts it above Fireworks, so
// the two swap places between two refreshes.
void swap_usage_providers() {
	std::string path = scratch_home + "/.omp/agent/sessions/fixture/usage.jsonl";
	// read_file drops the last newline, which the appended line needs
	write_file(path, read_file(path) + "\n"
		+ usage_line("g2", day_iso(0), "19:30:00",
			"anthropic-250k-prefer-using-this-one", "claude-opus-5-5",
			5000000, 0));
}

long long monotonic_ms() {
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// One JSON object member as a decimal, or 0 when it is absent.
double json_double(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return 0;
	}
	JsonNode *node = json_object_get_member(object, name);
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_VALUE
		? json_node_get_double(node) : 0;
}

// The record's provider objects, or empty when the collector said
// nothing. The parser that owns them stays for the run — one parse.
std::vector<JsonObject *> collector_providers() {
	static JsonParser *parser = nullptr;
	std::vector<JsonObject *> providers;
	const char *binary = std::getenv("WAYLAND_CANVAS_AGENT_USAGE");
	if (binary == nullptr || *binary == '\0') {
		check(false, "WAYLAND_CANVAS_AGENT_USAGE is unset: the collector "
			"is not on the scenario's PATH");
		return providers;
	}
	const char *const argv[] = {binary, static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	std::string out = child_output(child, 20);
	if (parser != nullptr) {
		g_object_unref(parser);
	}
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, out.c_str(),
		    static_cast<gssize>(out.size()), nullptr)) {
		check(false, "the collector's record is not JSON");
		return providers;
	}
	JsonNode *root = json_parser_get_root(parser);
	JsonObject *record = root != nullptr
		&& json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
	JsonNode *list = record != nullptr
		&& json_object_has_member(record, "providers")
			? json_object_get_member(record, "providers") : nullptr;
	if (list == nullptr || json_node_get_node_type(list) != JSON_NODE_ARRAY) {
		return providers;
	}
	JsonArray *array = json_node_get_array(list);
	for (guint i = 0; i < json_array_get_length(array); i++) {
		JsonNode *element = json_array_get_element(array, i);
		if (json_node_get_node_type(element) == JSON_NODE_OBJECT) {
			providers.push_back(json_node_get_object(element));
		}
	}
	return providers;
}

// The chip of the agents panel's switch that holds the selection, by the
// tint of its body, or -1. Each chip is read about its text's centre, the
// run the bar scan found, at the heights where only its own fill is drawn.
int selected_chip(cairo_surface_t *s) {
	for (std::size_t i = 0; i < agents_chip_runs.size(); i++) {
		wlr_box span{agents_box.x + 8
			+ static_cast<int>(agents_chip_runs[i].centre()) - 38,
			agents_box.y + 66, 76, 12};
		if (colour_count(s, span, chip_selected_dark) > 150) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// The widths of the meter fills in the panel's card: one entry per meter
// (a run of accent pixels a few rows tall), the session window first. An
// empty vector means no meter drew.
std::vector<int> meter_fills(cairo_surface_t *s, const wlr_box &card,
	uint32_t colour) {
	std::vector<int> fills;
	if (s == nullptr) {
		return fills;
	}
	int inner = card.width - 2 * UiThemePadding; // the meter track's width
	int best = 0, rows = 0;
	// The box carries no height (card_box names the top edge and width);
	// like text_bands the scan runs to the screen's bottom, where the
	// card itself ends.
	for (int y = card.y; y < screen_h; y++) {
		int lit = 0;
		for (int x = card.x + UiThemePadding;
				x < card.x + card.width - UiThemePadding; x++) {
			if (pixel_is(s, x, y, colour)) {
				lit++;
			}
		}
		if (lit > inner / 4) { // a meter row, not stray text pixels
			best = std::max(best, lit);
			rows++;
			continue;
		}
		if (rows > 0) { // the meter ended
			fills.push_back(best);
			best = 0;
			rows = 0;
		}
	}
	if (rows > 0) {
		fills.push_back(best);
	}
	return fills;
}

// The panel's rows of text: the bands with enough lit pixels to be a row.
int lit_rows(cairo_surface_t *s) {
	int rows = 0;
	for (const Band &b : text_bands(s, agents_box)) {
		if (b.lit >= 60) {
			rows++;
		}
	}
	return rows;
}

// The sign-in card a signed-out tab shows, in the attention colour.
wlr_box sign_in_card() {
	return {agents_box.x + 8, agents_box.y + 88, agents_box.width - 16, 52};
}

// A signed-out tab: the sign-in card, no meters, and the day and model
// figures still there.
bool signed_out_tab(cairo_surface_t *s) {
	return colour_count(s, sign_in_card(), attention_dark) > 30
		&& meter_fills(s, agents_box, accent_dark).empty() && lit_rows(s) >= 9;
}

// Lines in a text file: the stand-ins' logs, counted.
int log_lines(const std::string &path) {
	std::string text = read_file(path);
	if (text.empty()) {
		return 0;
	}
	int lines = 0;
	for (char c : text) {
		if (c == '\n') {
			lines++;
		}
	}
	return text.back() == '\n' ? lines : lines + 1;
}

// Writes one number of one plugin's settings into the scratch shell.json,
// keeping everything else it holds: an edit the host sees, so the shell
// reloads with it.
bool set_plugin_setting(const char *id, const char *key, int value) {
	JsonParser *parser = json_parser_new();
	std::string text = read_file(shell_config);
	bool ok = !text.empty()
		&& json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)
		&& json_node_get_node_type(json_parser_get_root(parser))
			== JSON_NODE_OBJECT;
	if (ok) {
		JsonObject *at = json_node_get_object(json_parser_get_root(parser));
		for (const char *step : {"plugins", id, "settings"}) {
			JsonNode *held = json_object_get_member(at, step);
			if (held == nullptr
					|| json_node_get_node_type(held) != JSON_NODE_OBJECT) {
				json_object_set_object_member(at, step, json_object_new());
			}
			at = json_node_get_object(json_object_get_member(at, step));
		}
		json_object_set_int_member(at, key, value);
		JsonGenerator *generator = json_generator_new();
		json_generator_set_root(generator, json_parser_get_root(parser));
		gchar *out = json_generator_to_data(generator, nullptr);
		write_file(shell_config, out);
		g_free(out);
		g_object_unref(generator);
	}
	g_object_unref(parser);
	return ok;
}

} // namespace

std::vector<harness::Step> agents_steps() {
	return {
		{"agents_phase_starts_in_the", [] {
			// the theme phase left: its pixel checks read the dark colours
			bool dark = server->theme.mode() == ThemeMode::dark;
			if (auto wait = shot_holds("agents-mode", bar_shows(dark ? shell_bg_dark : shell_bg_light))) {
				return *wait;
			}
			if (!dark) {
				click(theme_x, bar_mid_y()); // one switch back to dark
			}
			return after(700);
		}},
		{"agents_quiet_world", [] {
			if (auto wait = shot_holds("agents-hidden", [](cairo_surface_t *s) {
					return bar_shows(shell_bg_dark)(s) && bar_drawn()(s);
				})) {
				return *wait;
			}
			LayerSurface *bar = bar_layer();
			cairo_surface_t *s = load_png("agents-hidden");
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			bool dark = s != nullptr && bar != nullptr
				&& pixel_is(s, bar->geo.x + 4, bar->geo.y + bar->geo.height / 2, shell_bg_dark);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(dark, "the agents phase runs in the dark palette");
			check(bt_x != 0, "the bluetooth indicator's place is known");
			agents_base_runs = runs.size();
			watch_remember_bar();
			drop_usage_fixtures();
			reload_shell();
			return after(700);
		}},
		{"agents_reload_brings_the_widget", [] {
			// bluetooth, between the tray and it
			if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			// The counter carries into the runs wait below: resetting it here would
			// re-zero it on every retry tick and loop forever.
			if (!shot_landed("agents-live")) {
				return again(100);
			}
			cairo_surface_t *s = load_png("agents-live");
			LayerSurface *bar = s != nullptr ? bar_layer() : nullptr;
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// the collector may still be running
			if (auto wait = wait_for(runs.size() == agents_base_runs + 1, {.poll_ms = 1000, .budget_ms = 130000})) {
				return *wait;
			}
			check(runs.size() == agents_base_runs + 1,
				std::format("the agents widget appeared with usage ({} runs, "
					"was {})", runs.size(), agents_base_runs));
			// The run left of the bluetooth indicator's run is the widget:
			// its order sits between the tray's and bluetooth's.
			std::size_t bt = runs.size();
			for (std::size_t i = 0; i < runs.size(); i++) {
				if (bt_x >= runs[i].x0 && bt_x <= runs[i].x1) {
					bt = i;
				}
			}
			if (bt > 0) {
				agents_x = runs[bt - 1].centre();
			}
			check(bt > 0 && agents_x < bt_x,
				"the agents widget sits left of the bluetooth indicator");
			return after(500);
		}},
		{"agents_collectors_own_record_run_directly", [] {
			// fixture world: every family, figure and fault it should say
			std::vector<JsonObject *> providers = collector_providers();
			check(providers.size() == 4, std::format("four provider families "
				"({} of 4)", providers.size()));
			if (providers.size() != 4) {
				return go_to("agents_quiet_world_again", 300);
			}
			check(json_string(providers[0], "id") == "claude"
				&& json_string(providers[1], "id") == "gpt"
				&& json_string(providers[2], "id") == "fireworks"
				&& json_string(providers[3], "id") == "anthropic",
				"Claude, GPT, Fireworks and the gateway, in that order");
			check(json_string(providers[0], "title") == "Claude Code"
				&& json_string(providers[0], "tierLabel") == "Max 20x",
				"the Claude tab is Claude Code, its plan Max 20x");
			check(json_string(providers[3], "title") == "Claude (gateway)"
				&& json_string(providers[3], "name") == "Claude GW",
				"the gateway tab is Claude (gateway), chipped Claude GW");
			check(json_string(providers[0], "usageStatusText") == "",
				"the signed-in Claude tab reports no fault");
			JsonArray *limits = json_object_get_array_member(providers[0],
				"limits");
			check(limits != nullptr && json_array_get_length(limits) == 2,
				"the Claude tab carries both limits windows");
			if (limits != nullptr && json_array_get_length(limits) == 2) {
				JsonObject *session = json_array_get_object_element(limits, 0);
				JsonObject *weekly = json_array_get_object_element(limits, 1);
				check(json_string(session, "title") == "Session"
					&& json_string(weekly, "title") == "Weekly",
					"the windows are titled Session and Weekly");
				check(std::abs(json_double(session, "percent") - 0.37) < 0.002
					&& std::abs(json_double(weekly, "percent") - 0.684) < 0.002,
					"the probed shares are 37% and 68.4%");
				check(json_string(session, "resetsAt") != "",
					"the reset times normalise to ISO");
			}
			// The day figures the reference screenshot shows, today flagged
			// and labelled, bold in the panel below.
			JsonArray *days = json_object_get_array_member(providers[0], "days");
			check(days != nullptr && json_array_get_length(days) == 7,
				"seven days on the Claude tab");
			if (days != nullptr && json_array_get_length(days) == 7) {
				JsonObject *today = json_array_get_object_element(days, 6);
				JsonObject *wed = json_array_get_object_element(days, 4);
				JsonObject *thu = json_array_get_object_element(days, 5);
				check(json_object_get_boolean_member(today, "today")
					&& json_string(today, "label") == "Today"
					&& json_int(today, "tokens") == 811500
					&& json_string(today, "tokensText") == "811.5K",
					"today is flagged, labelled and 811.5K");
				check(json_int(wed, "tokens") == 246132594
					&& json_string(wed, "tokensText") == "246.1M",
					"two days ago is 246.1M");
				check(json_int(thu, "tokens") == 324632448
					&& json_string(thu, "tokensText") == "324.6M",
					"yesterday is 324.6M");
			}
			check(json_string(providers[0], "weekText") == "574.0M",
				"the week's tokens are 574.0M, one decimal like every figure");
			JsonArray *models = json_object_get_array_member(providers[0],
				"models");
			check(models != nullptr && json_array_get_length(models) == 2
				&& json_string(json_array_get_object_element(models, 0), "id")
					== "claude-opus-5-5"
				&& json_string(json_array_get_object_element(models, 0), "name")
					== "Opus 5.5"
				&& json_int(json_array_get_object_element(models, 0), "total")
					== 573878042,
				"Opus 5.5 leads the models at 573.9M, Haiku 4.5 behind it");
			check(json_string(providers[1], "usageStatusText")
					== "Waiting for auth"
				&& json_string(providers[1], "authHelpText")
					== "Run `codex login` to authenticate.",
				"the GPT tab is signed out, with the command that fixes it");
			JsonArray *gpt_limits = json_object_get_array_member(providers[1],
				"limits");
			check(gpt_limits != nullptr && json_array_get_length(gpt_limits) == 0,
				"a signed-out tab carries no limits");
			JsonArray *gpt_models = json_object_get_array_member(providers[1],
				"models");
			check(gpt_models != nullptr && json_array_get_length(gpt_models) >= 2,
				"the GPT tab still shows its figures");
			JsonArray *fw_limits = json_object_get_array_member(providers[2],
				"limits");
			check(fw_limits != nullptr && json_array_get_length(fw_limits) == 0
				&& json_string(providers[2], "title") == "Fireworks",
				"the Fireworks tab is tokens-only");
			usage_requests_base = log_lines(
				std::format("{}/usage-requests.log", shot_dir));
			check(usage_requests_base >= 1, "the bar's own start probe reached "
				"the stand-in");
			return after(500);
		}},
		{"agents_open_the_panel_on_the", [] {
			click(agents_x, bar_mid_y());
			return after(400);
		}},
		{"agents_panel_maps_belongs_to_the", [] {
			LayerSurface *panel = panel_layer("agents");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel != nullptr, "the agents panel appeared as a layer surface");
			if (panel == nullptr) {
				return go_to("agents_quiet_world_again", 300);
			}
			check(wl_resource_get_client(panel->wlr->surface->resource)
					== bar_client(),
				"the agents panel belongs to the bar client");
			if (auto wait = wait_for(server->seat->keyboard_state.focused_surface == panel->wlr->surface, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(server->seat->keyboard_state.focused_surface
					== panel->wlr->surface,
				"the agents panel holds the keyboard as it opens");
			agents_box = card_box(agents_x, agents_panel);
			return after(0);
		}},
		{"agents_panels_own_drawing_pixel_by", [] {
			// Claude mark, the two meters at their probed shares, the rows
			wlr_box mark{agents_box.x + UiThemePadding, agents_box.y + 16, 32, 32};
			int inner = agents_box.width - 2 * UiThemePadding;
			wlr_box chip_row{agents_box.x + 8, agents_box.y + 58, agents_box.width - 16, 28};
			// the picture holds once the mark, the meters at their shares, the
			// rows and the switch row's chips are all drawn
			auto drawn = [&](cairo_surface_t *s) {
				std::vector<int> fills = meter_fills(s, agents_box, accent_dark);
				int rows = 0;
				for (const Band &b : text_bands(s, agents_box)) {
					if (b.lit >= 60) {
						rows++;
					}
				}
				return colour_count(s, mark, claude_mark) > 20 && fills.size() == 2
					&& fills[0] >= inner / 4 && fills[0] <= inner / 2
					&& fills[1] > fills[0] && fills[1] >= inner * 0.55
					&& fills[1] <= inner * 0.82 && rows >= 11
					&& bar_runs(s, chip_row).size() >= 4;
			};
			if (auto wait = shot_holds("agents", drawn)) {
				return *wait;
			}
			cairo_surface_t *s = load_png("agents");
			int orange = colour_count(s, mark, claude_mark);
			check(orange > 20, std::format("the hero shows the drawn Claude "
				"mark ({} orange pixels)", orange));
			std::vector<int> fills = meter_fills(s, agents_box, accent_dark);
			check(fills.size() == 2, std::format("both limits meters drew "
				"({} of 2)", fills.size()));
			if (fills.size() == 2) {
				check(fills[0] >= inner / 4 && fills[0] <= inner / 2
					&& fills[1] > fills[0] && fills[1] >= inner * 0.55
					&& fills[1] <= inner * 0.82,
					std::format("the meters sit at their probed shares "
						"({} and {} of {})", fills[0], fills[1], inner));
			}
			std::vector<Band> bands = text_bands(s, agents_box);
			int rows = 0;
			for (const Band &b : bands) {
				if (b.lit >= 60) {
					rows++;
				}
			}
			check(rows >= 11, std::format("the panel's rows are all there "
				"({} rows: two limits, seven days, two models)", rows));
			// the day bars: each fill scaled to the busiest day, the second-busiest at
			// three quarters of it; the scan keeps to the bar's own column (named through
			// meter_fills' padding) so the weekday and value texts stay out of the count
			int track = agents_box.width - 2 * UiThemePadding - 116;
			wlr_box dayBars{agents_box.x + UiThemePadding + 50, agents_box.y,
				track + 2 * UiThemePadding, 0};
			std::vector<int> dayFills = meter_fills(s, dayBars, muted_dark);
			int busiestDay = 0, secondDay = 0;
			for (int fill : dayFills) {
				if (fill > busiestDay) {
					secondDay = busiestDay;
					busiestDay = fill;
				} else if (fill > secondDay) {
					secondDay = fill;
				}
			}
			check(dayFills.size() >= 2 && busiestDay >= track * 0.95
					&& secondDay >= track * 0.68 && secondDay <= track * 0.82,
				std::format("the day bars scale to the busiest day of the "
					"week ({} and {} of a {} px track)", busiestDay, secondDay,
					track));
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			return after(700);
		}},
		{"agents_provider_switch", [] {
			// the hero; GPT is the second of the four
			cairo_surface_t *s = load_png("agents");
			wlr_box chipRow{agents_box.x + 8, agents_box.y + 58,
				agents_box.width - 16, 28};
			agents_chip_runs = s != nullptr ? bar_runs(s, chipRow)
				: std::vector<Run>{};
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(agents_chip_runs.size() >= 4,
				std::format("the switch row carries the four chips "
					"({} runs)", agents_chip_runs.size()));
			if (agents_chip_runs.size() < 4) {
				return go_to("agents_quiet_world_again", 300);
			}
			// The chips run left to right: Claude, GPT, Fireworks, the gateway.
			// The runs name x inside the chip row's box, so the row's own
			// left edge joins them for the click.
			click(agents_box.x + 8 + agents_chip_runs[1].centre(),
				agents_box.y + 72);
			return after(400);
		}},
		{"agents_gpt_tab", [] {
			LayerSurface *panel = panel_layer("agents");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			if (auto wait = shot_holds("agents-gpt", signed_out_tab)) {
				return *wait;
			}
			cairo_surface_t *s = load_png("agents-gpt");
			int attention = colour_count(s, sign_in_card(), attention_dark);
			check(attention > 30, std::format("the GPT tab shows its sign-in "
				"card ({} attention pixels)", attention));
			check(meter_fills(s, agents_box, accent_dark).empty(),
				"a signed-out tab draws no meters");
			int rows = lit_rows(s);
			check(rows >= 9, std::format("the GPT tab keeps its day and model "
				"figures ({} rows)", rows));
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			return after(700);
		}},
		{"agents_fireworks_tab", [] {
			click(agents_box.x + 8 + agents_chip_runs[2].centre(),
				agents_box.y + 72);
			return after(400);
		}},
		{"agents_no_card_no_meters_the", [] {
			LayerSurface *panel = panel_layer("agents");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			wlr_box card = sign_in_card();
			wlr_box mark{agents_box.x + UiThemePadding, agents_box.y + 16, 32, 32};
			if (auto wait = shot_holds("agents-fireworks", [&](cairo_surface_t *s) {
					return colour_count(s, card, attention_dark) < 10
						&& meter_fills(s, agents_box, accent_dark).empty()
						&& colour_count(s, mark, accent_dark) > 20;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("agents-fireworks");
			check(colour_count(s, card, attention_dark) < 10,
				"a token-only tab draws no sign-in card");
			check(meter_fills(s, agents_box, accent_dark).empty(),
				"a token-only tab draws no meters");
			check(colour_count(s, mark, accent_dark) > 20,
				"a token-only family shows the star mark");
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			return after(700);
		}},
		{"agents_close_the_panel", [] {
			click(20, screen_h - 20);
			return after(400);
		}},
		{"agents_closed", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the agents panel closed");
			write_usage_payload("95.0", "72.0");
			// Between the two refreshes the gateway outranks Fireworks: the
			// collector's own order is read, so the swap is known to be real.
			swap_usage_providers();
			std::vector<JsonObject *> ranked = collector_providers();
			check(ranked.size() == 4 && json_string(ranked[2], "id") == "anthropic"
					&& json_string(ranked[3], "id") == "fireworks",
				"the collector now ranks the gateway above Fireworks");
			return after(20000); // past the probe-reuse window, so a reopen re-asks
		}},
		{"agents_reopen", [] {
			click(agents_x, bar_mid_y());
			return after(400);
		}},
		{"agents_panel_kept_the_provider_it", [] {
			// collector ranked the gateway above it between the refreshes, so
			// Fireworks is the fourth chip now and the selection has to have
			// moved with it, not stayed on the third place
			LayerSurface *panel = panel_layer("agents");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			wlr_box mark{agents_box.x + UiThemePadding, agents_box.y + 16, 32, 32};
			auto star_shown = [&](cairo_surface_t *s) {
				return colour_count(s, mark, accent_dark) > 20
					&& colour_count(s, mark, claude_mark) < 10;
			};
			// the panel-open refresh is still running
			if (auto wait = shot_holds("agents-reordered", [&](cairo_surface_t *s) {
					return selected_chip(s) == 3 && star_shown(s);
				}, {.poll_ms = 400, .budget_ms = 24000})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("agents-reordered");
			int selected = selected_chip(s);
			bool star = star_shown(s);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(selected == 3, std::format("the selection moved with Fireworks "
				"to the fourth chip when the refresh reordered the families "
				"(chip {} holds it)", selected));
			check(star, "the hero still shows Fireworks, not the gateway that "
				"took its place");
			// the refreshed limits live on the Claude tab, so switch to it
			click(agents_box.x + 8 + agents_chip_runs[0].centre(),
				agents_box.y + 72);
			return after(400);
		}},
		{"agents_refresh_probe_happened", [] {
			// nearly-spent window turned the attention colour
			LayerSurface *panel = panel_layer("agents");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			if (auto wait = shot_holds("agents-refreshed", [](cairo_surface_t *s) {
					return !meter_fills(s, agents_box, attention_dark).empty()
						&& !meter_fills(s, agents_box, accent_dark).empty();
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("agents-refreshed");
			int requests = log_lines(
				std::format("{}/usage-requests.log", shot_dir));
			check(requests > usage_requests_base,
				std::format("the reopen probed again ({} requests, was {})",
					requests, usage_requests_base));
			check(!meter_fills(s, agents_box, attention_dark).empty(),
				"the 95% session window draws in the attention colour");
			check(!meter_fills(s, agents_box, accent_dark).empty(),
				"the weekly window still draws in the accent");
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			return after(700);
		}},
		{"agents_close", [] {
			click(20, screen_h - 20);
			return after(400);
		}},
		{"agents_closed_2", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			std::error_code ec;
			std::filesystem::remove(scratch_home + "/.claude/.credentials.json", ec);
			watch_remember_bar();
			reload_shell();
			return after(700);
		}},
		{"agents_reloaded_bar_keeps_the_widget", [] {
			// As at the first reload: the bar returns at once, the widget only
			// with the collector's answer — the codex handshake takes seconds —
			// so the count is waited for, never assumed.
			if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			if (!shot_landed("agents-signedout-bar")) {
				return again(100);
			}
			cairo_surface_t *s = load_png("agents-signedout-bar");
			LayerSurface *bar = s != nullptr ? bar_layer() : nullptr;
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// the collector may still be answering
			if (auto wait = wait_for(runs.size() == agents_base_runs + 1, {.poll_ms = 1000, .budget_ms = 130000})) {
				return *wait;
			}
			check(runs.size() == agents_base_runs + 1,
				std::format("the widget stays with its figures, signed out or "
					"not ({} runs)", runs.size()));
			return after(500);
		}},
		{"agents_open_the_signed_out_panel", [] {
			click(agents_x, bar_mid_y());
			return after(400);
		}},
		{"agents_card_and_the_attention_meta", [] {
			LayerSurface *panel = panel_layer("agents");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			wlr_box hero{agents_box.x, agents_box.y, agents_box.width, 54};
			if (auto wait = shot_holds("agents-signedout", [&](cairo_surface_t *s) {
					return signed_out_tab(s) && colour_count(s, hero, attention_dark) > 10;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("agents-signedout");
			check(colour_count(s, sign_in_card(), attention_dark) > 30,
				"the signed-out Claude tab shows its sign-in card");
			check(colour_count(s, hero, attention_dark) > 10,
				"the fault names itself in the hero's meta line");
			check(meter_fills(s, agents_box, accent_dark).empty(),
				"no probe, no meters");
			int rows = lit_rows(s);
			check(rows >= 9, std::format("the figures stay beside the card "
				"({} rows)", rows));
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			return after(700);
		}},
		{"agents_close_2", [] {
			click(20, screen_h - 20);
			return after(400);
		}},
		{"agents_closed_3", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			click(theme_x, bar_mid_y());
			return after(700);
		}},
		{"agents_light_bar_confirmed_the_panel", [] {
			if (auto wait = shot_holds("agents-light-bar", bar_shows(shell_bg_light))) {
				return *wait;
			}
			LayerSurface *bar = bar_layer();
			cairo_surface_t *s = load_png("agents-light-bar");
			check(bar != nullptr && s != nullptr
				&& pixel_is(s, bar->geo.x + 4, bar->geo.y + bar->geo.height / 2, shell_bg_light),
				"the bar switched to the light palette");
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			click(agents_x, bar_mid_y());
			return after(400);
		}},
		{"agents_light_panel", [] {
			LayerSurface *panel = panel_layer("agents");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			wlr_box mark{agents_box.x + UiThemePadding, agents_box.y + 16, 32, 32};
			int edge_x = agents_box.x + agents_box.width - 6;
			int track = agents_box.width - 2 * UiThemePadding - 116;
			wlr_box dayBars{agents_box.x + UiThemePadding + 50, agents_box.y,
				track + 2 * UiThemePadding, 0};
			if (auto wait = shot_holds("agents-light", [&](cairo_surface_t *s) {
					return pixel_is(s, edge_x, agents_box.y + 70, panel_light)
						&& colour_count(s, mark, claude_mark) > 20
						&& !meter_fills(s, dayBars, muted_light).empty();
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("agents-light");
			check(pixel_is(s, edge_x, agents_box.y + 70, panel_light),
				"the panel card is the light surface");
			check(colour_count(s, mark, claude_mark) > 20,
				"the Claude mark keeps its colour in the light");
			// The day bars carry the light palette's own muted fill.
			check(!meter_fills(s, dayBars, muted_light).empty(),
				"the day bars keep drawing in the light palette");
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			return after(700);
		}},
		{"agents_close_3", [] {
			click(20, screen_h - 20);
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			run_msg("theme.set", R"({"mode":"dark"})", "agents-dark-again");
			return after(900); // the palette settles before the reload joins it
		}},
		{"agents_setting_written_into_shell_json", [] {
			agents_config_kept = read_file(shell_config);
			agents_runs_before = codex_runs();
			agents_first_run_ms = 0;
			watch_remember_bar();
			check(set_plugin_setting("canvas.agents", "refreshIntervalSec", 2),
				"refreshIntervalSec: 2 written into the scratch shell.json");
			return after(100);
		}},
		{"agents_reloaded_widget_runs_at_its", [] {
			// 4 s on its own: the runs are watched every 100 ms from the write
			int runs = codex_runs() - agents_runs_before;
			long long now = monotonic_ms();
			if (runs >= 1 && agents_first_run_ms == 0) {
				agents_first_run_ms = now;
			}
			if (auto wait = wait_for(runs >= 2, {.poll_ms = 100, .budget_ms = 20000})) {
				return *wait;
			}
			check(runs >= 2 && now - agents_first_run_ms <= 4000,
				std::format("the collector ran again within 4 s on the 2 s "
					"refreshIntervalSec ({} runs after the reload, the last {} ms "
					"after the first seen)", runs, now - agents_first_run_ms));
			watch_remember_bar();
			write_file(shell_config, agents_config_kept); // the setting out again
			return after(700);
		}},
		{"agents_bar_reloaded_on_the_restored", [] {
			if (auto wait = wait_for(shell_reloaded() && bar_layer()->geo.height >= 20, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			return after(300);
		}},
		{"agents_empty_the_world_reload", [] {
			remove_usage_fixtures();
			watch_remember_bar();
			reload_shell();
			return after(700);
		}},
		{"agents_quiet_world_again", [] {
			// stand-ins received, and that nothing real was ever reached
			if (auto wait = wait_for(shell_reloaded() && bar_layer()->geo.height >= 20, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			if (!shot_landed("agents-quiet")) {
				return again(100);
			}
			cairo_surface_t *s = load_png("agents-quiet");
			LayerSurface *bar = s != nullptr ? bar_layer() : nullptr;
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// The rebuilt bar fills in as each widget's source answers again
			// (the weather waits for its first fetch): look again until it has.
			if (auto wait = wait_for(runs.size() == agents_base_runs, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(runs.size() == agents_base_runs,
				std::format("the widget hid again with the world empty "
					"({} runs, was {} before it)", runs.size(),
					agents_base_runs));
			// The probes: the panel's own token at the stand-in, and the
			// codex RPC the fake app-server answered.
			std::string requests = read_file(
				std::format("{}/usage-requests.log", shot_dir));
			check(requests.find("fixture-token") != std::string::npos,
				"the probe carried the fixture's own token");
			check(requests.find("oauth-2025-04-20") != std::string::npos,
				"the probe carried the oauth beta header");
			std::string codex = read_file(
				std::format("{}/codex-standin.log", shot_dir));
			check(codex.find("initialize") != std::string::npos
				&& codex.find("account/read") != std::string::npos
				&& codex.find("account/rateLimits/read") != std::string::npos,
				"the codex stand-in answered the handshake and both reads");
			// Nothing real: no request ever left for the real endpoint, and
			// the bar's own HOME is the scratch one, not the machine's.
			std::string curls = read_file(
				std::format("{}/curl-standin.log", shot_dir));
			check(curls.find("api.anthropic.com") == std::string::npos,
				"no request ever named the real Anthropic endpoint");
			check(curls.find("usage") != std::string::npos,
				"the usage probe went through the curl stand-in");
			pid_t shell_pid = 0;
			if (bar_client() != nullptr) {
				wl_client_get_credentials(bar_client(), &shell_pid, nullptr,
					nullptr);
			}
			check(shell_pid > 0
					&& proc_env_value(shell_pid, "HOME") == scratch_home,
				"the bar's HOME is the scenario's scratch, never the "
				"machine's");
			check(panel_layers() == 0, "no panel outlived the agents phase");
			check(toolbar_layer() == nullptr,
				"no toolbar outlived the agents phase");
			check(overview_settled(false), "the overview stayed closed");
			return after(100);
		}},
	};
}

} // namespace bar
