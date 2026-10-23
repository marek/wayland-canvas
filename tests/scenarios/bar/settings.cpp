// The settings window: the gear, its tabs and what each writes.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

std::size_t settings_base_runs = 0; // the bar's runs before the agents widget

int settings_card_y = 0; // the card on the Bar tab
double settings_agents_switch_y = 0; // the agents row's switch, the same tab
int views_before = 0; // the views on the canvas before the chord check
int terminals_before = 0; // the foot windows among them
int settings_terminal_y = 0; // the Terminal row's label line, on the Shortcuts tab
int settings_chip_before = 0; // its chord chip's width before the recording
int settings_first_top = 0; // the Gestures tab's first preset row
std::string settings_conf_before; // canvas.conf before a write its directory refuses
bool settings_esc_sent = false; // the Escape that closes the panel went out
std::string keys_marker;    // what a plain key typed into the keys terminal lands in
std::string command_marker; // the file the Commands row's command touches
std::string settings_last_png; // the last list shot, to tell when a scroll has stopped
int settings_draft_chip = 0; // the draft command row's chip width while it shows None

// Wheel clicks over (x, y): positive scrolls the content down, negative up.
void scroll(double x, double y, int clicks) {
	move_to(x, y);
	int sign = clicks < 0 ? -1 : 1;
	for (int i = 0; i < std::abs(clicks); i++) {
		wheel(120 * sign);
	}
}

constexpr int settings_action = 2 * settings_row; // a shortcuts row: label and chords

// The Bar tab's column of content: the three sections.
int settings_bar_column() {
	return settings_section_height(1, 0)              // Left: the logo
		+ settings_section_height(2, 0)                // Center: clock, weather
		+ settings_section_height(9, 1)                // Right: tray to theme
		+ 2 * settings_gap;
}

// The Gestures tab's column of content: the swipe presets, the pinch
// switch and the five direction switches, a header over each section.
int settings_gestures_column() {
	return settings_section_height(0, 3)  // Swipes: the three presets
		+ settings_section_height(0, 1)    // Pinch: scale a desktop
		+ settings_section_height(0, 5)    // Direction: a switch each
		+ 2 * settings_gap;
}

// The Toolbar tab's column of content: a header and the toolbar's four edge
// choices, then the app bar's header, its switch row and its own four edges
// (docs/intents/shell/app-bar.md); every row is a two-line one.
int settings_toolbar_column() {
	return settings_section_height(0, 4)   // the toolbar's edges
		+ settings_section_height(0, 5)     // the app bar: the switch, 4 edges
		+ settings_gap;
}

// The toolbar strip's button bands: the cluster of lit bands closest to
// the output's vertical centre in the strip's column — its own glyphs
// are the only lit pixels in it.
std::vector<Band> settings_strip_bands(cairo_surface_t *s, int x) {
	std::vector<Band> all = bands_in(s, {x, 0, toolbar_button, screen_h});
	std::vector<Band> best;
	double best_distance = screen_h;
	for (std::size_t i = 0; i < all.size();) {
		std::vector<Band> cluster{all[i]};
		for (std::size_t j = i + 1; j < all.size()
				&& all[j].top - cluster.back().bottom <= toolbar_button; j++) {
			cluster.push_back(all[j]);
		}
		double distance = std::abs((cluster.front().top + cluster.back().bottom) / 2.0
			- screen_h / 2.0);
		if (cluster.size() > best.size()
				|| (cluster.size() == best.size() && distance < best_distance)) {
			best = cluster;
			best_distance = distance;
		}
		i += cluster.size();
	}
	return best;
}

constexpr uint32_t chip_dark = 0x272833ff;     // UiTheme.fill over the panel: a key chip
constexpr uint32_t chip_hover_dark = 0x383943ff; // the same chip on its row's hover fill

// The tab labels as lit runs of the tab row: one per tab, each centred in
// its share of the row.
bool settings_tab_labels_centred(cairo_surface_t *s, int card_y) {
	// One cell at a time: a row read whole would run two labels into one.
	for (int i = 0; i < settings_tabs; i++) {
		auto [cx, cy] = settings_tab_point(i, card_y);
		std::vector<Run> runs = bar_runs(s,
			{int(cx - settings_tab_w / 2.0), int(cy - settings_row / 2.0),
				settings_tab_w, settings_row});
		if (runs.empty()) {
			return false;
		}
		double left = runs.front().x0, right = runs.back().x1;
		if (std::abs((left + right) / 2 - settings_tab_w / 2.0) > 6) {
			return false;
		}
	}
	return true;
}

// The Bar tab's widget rows from the card's top: each row's centre, top
// to bottom, in the bar's own section order.
std::vector<double> settings_bar_rows(int card_y) {
	std::vector<std::vector<int>> sections = {
		{settings_row},                                   // Left: the logo
		{settings_row, settings_row},                     // Center: clock, weather
		{settings_row, settings_choice},                  // Right: tray, agents
	};
	// bluetooth, network, audio, brightness, notifications, power, theme
	for (int i = 0; i < 8; i++) {
		sections[2].push_back(settings_row);
	}
	std::vector<double> centres;
	int y = card_y + settings_pad + settings_tabs_h + settings_gap;
	for (const std::vector<int> &rows : sections) {
		y += settings_header + settings_gap;
		for (int height : rows) {
			centres.push_back(y + height / 2.0);
			y += height + settings_gap;
		}
	}
	return centres;
}

// Which of `count` exclusive choice rows show their dot: bit i is the
// i-th row's, the accent at the dot's place on the row's right.
unsigned settings_dots(cairo_surface_t *s, int first_top, int count) {
	int dot_x = settings_card_x + settings_pad + settings_panel
		- 2 * settings_pad - 8 - 4;
	unsigned lit = 0;
	for (int i = 0; i < count; i++) {
		int y = first_top + i * (settings_choice + settings_gap) + settings_choice / 2;
		if (pixel_is(s, dot_x, y, accent_dark)) {
			lit |= 1u << i;
		}
	}
	return lit;
}

// True when the running session's resolved keys for the global `action`
// hold `chord`: what the compositor applied, not what a file says.
bool holds_chord(std::string_view action, const settings::Chord &chord) {
	const std::vector<settings::ActionDef> &registry = settings::registry();
	for (std::size_t i = 0; i < registry.size(); i++) {
		if (registry[i].scope != settings::Scope::global
				|| registry[i].name != action) {
			continue;
		}
		for (settings::Chord held : server->conf.resolved.keys[i]) {
			if (held == chord) {
				return true;
			}
		}
	}
	return false;
}

// The App launcher row's label line, the row above Terminal: it carries no
// note, so its third line is the recording prompt and nothing else.
int settings_launcher_y() {
	return settings_terminal_y - settings_action - settings_gap;
}

// True when a shot shows the App launcher row's third line, under its
// chords: where the row says it is recording.
bool settings_prompt_shown(const char *name) {
	cairo_surface_t *s = load_png(name);
	double top = settings_launcher_y();
	double note_y = top + 1.5 * settings_row + settings_detail / 2.0;
	std::vector<Band> lines = s != nullptr ? bands_in(s,
		{settings_card_x + 8, int(top) - 20, settings_label_w,
			3 * settings_row}) : std::vector<Band>{};
	auto note = settings_band_at(lines, note_y);
	bool shown = note != lines.end() && std::abs(note->centre() - note_y) <= 8;
	cairo_surface_destroy(s);
	return shown;
}

// The Shortcuts list on the tall card, scrolled to its end: its bottom
// edge, where the last row sits. The list is as tall as the output
// leaves room for (SettingsPanel.qml's listMaxHeight).
constexpr int settings_list_bottom = 8 + settings_pad + settings_tabs_h + settings_gap
	+ screen_h - 2 * 8 - 2 * settings_pad - settings_tabs_h - 3 * settings_gap
	- settings_footer - 1;

constexpr double settings_last_row_y = settings_list_bottom - settings_row / 2.0;

// Whether the shot is byte for byte the one this was last called with:
// the list stopped moving. The first call after a clear says no.
bool settings_png_repeats(const char *name) {
	std::string now = read_file(std::format("{}/{}.png", shot_dir, name));
	bool same = !now.empty() && now == settings_last_png;
	settings_last_png = std::move(now);
	return same;
}

// The command row above the Add row once the list is scrolled to its end.
constexpr double settings_command_y = settings_last_row_y - settings_row - settings_gap;

// How many of `count` stacked choice rows carry a text band where the
// card lays them.
int settings_choices_labelled(const std::vector<Band> &rows, int first_top,
	int count) {
	int n = 0;
	for (int i = 0; i < count; i++) {
		double y = first_top + i * (settings_choice + settings_gap)
			+ settings_choice / 2.0;
		auto band = settings_band_at(rows, y);
		n += band != rows.end() && std::abs(band->centre() - y) <= 12;
	}
	return n;
}

// The width of the key chip that starts at the row's left edge, measured
// along line `y` from four pixels in: the run of chip-fill pixels. The
// pointer rests on the row it just clicked, so the fill may be the
// hovered one; the run is of whichever the first pixel is.
int settings_chip_width(cairo_surface_t *s, int y) {
	int x = settings_card_x + settings_pad + 8 + 4;
	uint32_t fill = pixel_is(s, x, y, chip_dark) ? chip_dark
		: pixel_is(s, x, y, chip_hover_dark) ? chip_hover_dark : 0;
	int n = 0;
	while (fill != 0 && x + n < screen_w && pixel_is(s, x + n, y, fill)) {
		n++;
	}
	return n == 0 ? 0 : n + 4;
}

// The Gestures tab's direction row `index`, centred: under the three
// presets and the pinch row with their headers.
double settings_direction_y(int index) {
	double pinch_y = settings_first_top + 3 * (settings_choice + settings_gap)
		+ settings_header + settings_gap + settings_choice / 2.0;
	return pinch_y + settings_choice + settings_header + 2 * settings_gap
		+ index * (settings_choice + settings_gap);
}

// canvas.conf's value on `name`'s line, "" when the file gives it none:
// whitespace around the = is free, so both sides are trimmed.
std::string conf_value(const std::string &name) {
	std::string text = read_file(canvas_conf);
	std::size_t start = 0;
	while (start < text.size()) {
		std::size_t end = text.find('\n', start);
		std::string line = text.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		std::size_t first = line.find_first_not_of(" \t\r");
		if (first != std::string::npos) {
			std::size_t last = line.find_last_not_of(" \t\r");
			line = line.substr(first, last - first + 1);
			if (line.starts_with(name)) {
				std::size_t after = line.find_first_not_of(" \t", name.size());
				if (after != std::string::npos && line[after] == '=') {
					std::size_t vfirst = line.find_first_not_of(" \t", after + 1);
					if (vfirst == std::string::npos) {
						return "";
					}
					std::size_t vlast = line.find_last_not_of(" \t\r");
					return line.substr(vfirst, vlast - vfirst + 1);
				}
			}
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return "";
}

// How many lines of canvas.conf carry `text` as their whole value — a
// chord counting must not find "Super+T" inside "Super+Tab".
int conf_count(const std::string &text) {
	std::string raw = read_file(canvas_conf);
	int n = 0;
	std::size_t start = 0;
	while (start < raw.size()) {
		std::size_t end = raw.find('\n', start);
		std::string line = raw.substr(start,
			(end == std::string::npos ? raw.size() : end) - start);
		std::size_t equals = line.find('=');
		if (equals != std::string::npos) {
			std::size_t first = line.find_first_not_of(" \t", equals + 1);
			std::size_t last = line.find_last_not_of(" \t\r");
			if (first != std::string::npos
					&& line.substr(first, last - first + 1) == text) {
				n++;
			}
		}
		start = end == std::string::npos ? raw.size() : end + 1;
	}
	return n;
}

// The config's toolbar.position, "" when it holds none.
std::string shell_toolbar_position() {
	std::string out;
	JsonParser *parser = json_parser_new();
	if (json_parser_load_from_file(parser, shell_config.c_str(), nullptr)) {
		JsonNode *root_node = json_parser_get_root(parser);
		JsonObject *root = root_node != nullptr
			&& json_node_get_node_type(root_node) == JSON_NODE_OBJECT
			? json_node_get_object(root_node) : nullptr;
		if (root != nullptr && json_object_has_member(root, "toolbar")
				&& json_node_get_node_type(json_object_get_member(root, "toolbar"))
					== JSON_NODE_OBJECT) {
			out = json_string(json_node_get_object(
				json_object_get_member(root, "toolbar")), "position");
		}
	}
	g_object_unref(parser);
	return out;
}

// The mapped views on the canvas, by count: the terminal check watches
// a new one appear.
int mapped_views() {
	int n = 0;
	for (View *v : server->views) {
		if (v->mapped) {
			n++;
		}
	}
	return n;
}

// The mapped views of one app id: the terminal action's foot windows.
int mapped_views_of(std::string_view app_id) {
	int n = 0;
	for (View *v : server->views) {
		if (v->mapped && v->shell_app_id() != nullptr && app_id == v->shell_app_id()) {
			n++;
		}
	}
	return n;
}

// Where the settings card's top-left corner belongs for the toolbar as it is placed now
// (PanelCard: one margin past the strip's inner side, kept inside the output): beside a
// column and level with the gear, below or above a row and centred on the gear. `height` is
// the card's own.
std::pair<int, int> settings_card_origin(int height) {
	LayerSurface *toolbar = toolbar_layer();
	wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
	double gear = 4 * toolbar_button + toolbar_button / 2.0; // along the strip
	if (strip.width == toolbar_button) { // a column
		int x = strip.x > screen_w / 2 ? strip.x - beside_gap - settings_panel
			: strip.x + toolbar_button + beside_gap;
		return {x, settings_card_top(height, strip.y + gear)};
	}
	int x = int(std::round(std::max(8.0, std::min(strip.x + gear - settings_panel / 2.0,
		double(screen_w - settings_panel - 8)))));
	int y = strip.y > screen_h / 2
		? std::max(8, strip.y - beside_gap - height)
		: std::min(strip.y + toolbar_button + beside_gap, screen_h - height - 8);
	return {x, y};
}

// The Toolbar tab as a shot shows it, against the toolbar as placed now:
// where the card belongs (settings_card_x follows it), whether its tab
// row sits there with the Toolbar tab chosen, and which edge choices
// show their dot.
struct ToolbarCard {
	int card_y = 0;
	int first_top = 0; // the first edge choice's top
	bool placed = false;
	unsigned dots = 0;
};

ToolbarCard settings_toolbar_card(const char *shot_name) {
	auto [x, y] = settings_card_origin(settings_card_height(settings_toolbar_column()));
	settings_card_x = x;
	ToolbarCard card;
	card.card_y = y;
	card.first_top = y + settings_pad + settings_tabs_h + settings_gap
		+ settings_header + settings_gap;
	cairo_surface_t *s = load_png(shot_name);
	if (s != nullptr) {
		card.placed = settings_chosen_tab(s, y) == 3
			&& settings_tab_labels_centred(s, y);
		card.dots = settings_dots(s, card.first_top, 4);
		cairo_surface_destroy(s);
	}
	return card;
}

} // namespace

std::vector<harness::Step> settings_steps() {
	return {
		{"settings_quiet_bar_first", [] {
			if (auto wait = shot_holds("settings-quiet-bar", bar_drawn())) {
				return *wait;
			}
			check(toolbar_layer() == nullptr,
				"the settings phase starts out of the overview");
			cairo_surface_t *s = load_png("settings-quiet-bar");
			LayerSurface *bar = bar_layer();
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(runs.size() >= 4, std::format("the bar shows its widgets "
				"before the settings phase ({} runs)", runs.size()));
			check(bt_x != 0, "the bluetooth indicator's place is known");
			settings_base_runs = runs.size();
			watch_remember_bar();
			drop_usage_fixtures();
			reload_shell();
			return after(700);
		}},
		{"settings_reload_brings_the_agents_widget", [] {
			if (auto wait = wait_for(!((bar_layer() == nullptr || settings_shell_kept())), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			// The counter carries into the runs wait below: resetting it here
			// would re-zero it on every retry tick and loop forever.
			if (!shot_landed("settings-agents-bar")) {
				return again(100);
			}
			cairo_surface_t *s = load_png("settings-agents-bar");
			LayerSurface *bar = s != nullptr ? bar_layer() : nullptr;
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// the collector may still be running
			if (auto wait = wait_for(runs.size() == settings_base_runs + 1, {.poll_ms = 1000, .budget_ms = 230000})) {
				return *wait;
			}
			check(runs.size() == settings_base_runs + 1,
				std::format("the agents widget is back with usage ({} runs, was {})",
					runs.size(), settings_base_runs));
			agents_x = 0;
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
			chord(KEY_TAB, mods::super); // Super+Tab opens the overview
			return after(400);
		}},
		{"settings_toolbar_maps_with_the_overview", [] {
			LayerSurface *toolbar = toolbar_layer();
			if (auto wait = wait_for(toolbar != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(toolbar != nullptr, "the overview mapped the toolbar");
			if (toolbar != nullptr) {
				check(wl_resource_get_client(toolbar->wlr->surface->resource) == bar_client(),
					"the toolbar belongs to the shell client");
			}
			shot("settings-toolbar");
			return after(700);
		}},
		{"settings_gear_the_strips_last_button", [] {
			cairo_surface_t *s = load_png("settings-toolbar");
			std::vector<Band> buttons = settings_strip_bands(s, strip_left);
			cairo_surface_destroy(s);
			// the slide-in may finish
			if (auto wait = wait_for(buttons.size() >= 5, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-toolbar");
				return *wait;
			}
			check(buttons.size() >= 5, std::format("the toolbar holds the settings "
				"gear beside its own buttons ({} glyph bands)", buttons.size()));
			if (buttons.size() < 5) {
				return end_run("the toolbar does not hold the settings gear, so no "
					"position can be trusted");
			}
			settings_gear_y = buttons[4].centre();
			click(strip_left + toolbar_button / 2.0, settings_gear_y);
			return after(400);
		}},
		{"settings_panel_maps_as_a_layer", [] {
			LayerSurface *panel = panel_layer("settings");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel != nullptr, "the gear opened the settings panel through "
				"the common trigger");
			if (panel != nullptr) {
				check(wl_resource_get_client(panel->wlr->surface->resource) == bar_client(),
					"the settings panel belongs to the shell client");
			}
			// The bar this panel opened under: nothing the phase does may reload it.
			watch_remember_bar();
			shot("settings-panel");
			return after(700);
		}},
		{"settings_card_beside_the_strip_its", [] {
			cairo_surface_t *s = load_png("settings-panel");
			settings_card_x = settings_card_left(false);
			// The Shortcuts list is taller than the output, so the card sits
			// clamped at the top margin.
			bool tabs_ok = s != nullptr && settings_tab_labels_centred(s, 8);
			int chosen = s != nullptr ? settings_chosen_tab(s, 8) : -1;
			cairo_surface_destroy(s);
			if (auto wait = wait_for(!((!tabs_ok || chosen != 0)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-panel");
				return *wait;
			}
			check(tabs_ok, "the settings card shows tall, its tabs clamped "
				"at the top margin");
			check(chosen == 0, "the Shortcuts tab is the one chosen when the panel "
				"opens");
			// The Bar tab is the row's fourth share.
			{
				auto [tx, ty] = settings_tab_point(4, 8); // the Bar tab
				click(tx, ty);
			}
			return after(400);
		}},
		{"settings_bar_tab_drawn", [] {
			shot("settings-bar-tab");
			return after(700);
		}},
		{"settings_agents_rows_switch", [] {
			cairo_surface_t *s = load_png("settings-bar-tab");
			int column = settings_bar_column();
			settings_card_y = settings_card_top(settings_card_height(column),
				settings_gear_y);
			std::vector<double> widgets = settings_bar_rows(settings_card_y);
			// Labels only: the switches would light a row by themselves.
			std::vector<Band> rows = bands_in(s,
				{settings_card_x + 8, settings_card_y, settings_label_w,
					settings_card_height(column)});
			int chosen = s != nullptr ? settings_chosen_tab(s, settings_card_y) : -1;
			int labelled = 0, on = 0;
			for (double y : widgets) {
				auto band = settings_band_at(rows, y);
				labelled += band != rows.end() && std::abs(band->centre() - y) <= 12;
				on += s != nullptr && settings_switch_is(s, y, true);
			}
			// The agents row: second of the Right section, the one detailed
			// row: its label line sits above the row's centre and its detail
			// line below, two bands where a plain row has one.
			settings_agents_switch_y = widgets[4];
			bool label_line = false, detail_line = false;
			for (const Band &band : rows) {
				double d = band.centre() - settings_agents_switch_y;
				label_line = label_line || (d < -2 && d > -18);
				detail_line = detail_line || (d > 2 && d < 18);
			}
			bool detailed = label_line && detail_line;
			int marked = s != nullptr ? colour_count(s, {settings_card_x + 8,
				int(settings_agents_switch_y) - 18, settings_label_w, 36},
				attention_dark) : 0;
			cairo_surface_destroy(s);
			int expected = int(widgets.size());
			if (auto wait = wait_for(!((chosen != 4 || labelled != expected || on != expected || !detailed || marked >= 10)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-bar-tab");
				return *wait;
			}
			check(chosen == 4, "the Bar tab is the one chosen");
			check(labelled == expected, std::format("the Bar tab lists every widget "
				"by section, a label on each row ({} of {})", labelled, expected));
			check(on == expected, std::format("every widget's switch is on while "
				"none is hidden ({} of {})", on, expected));
			check(detailed && marked < 10, "the agents row sits where the panel "
				"lays it, its detail line naming when it shows, its label in the "
				"normal colour");
			click(settings_card_x + settings_pad
				+ settings_panel - 2 * settings_pad - 8 - settings_switch_w / 2.0,
				settings_agents_switch_y);
			return after(400);
		}},
		{"settings_write", [] {
			std::vector<std::string> hidden = shell_bar_hidden();
			bool only_agents = hidden.size() == 1 && hidden[0] == "canvas.agents/agents";
			if (auto wait = wait_for(only_agents, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(only_agents, "hiding the agents widget wrote it, and only it, "
				"into bar.hidden in shell.json");
			shot("settings-bar-hidden");
			return after(500);
		}},
		{"settings_bars_pixels_lose_the_widget", [] {
			cairo_surface_t *s = load_png("settings-bar-hidden");
			LayerSurface *bar = bar_layer();
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			// The agents row's switch is off, every other still on, and the
			// hidden widget's label is marked in the attention colour.
			std::vector<double> widgets = settings_bar_rows(settings_card_y);
			bool agents_off = s != nullptr
				&& settings_switch_is(s, settings_agents_switch_y, false);
			int others_on = 0;
			for (double y : widgets) {
				others_on += y != settings_agents_switch_y && s != nullptr
					&& settings_switch_is(s, y, true);
			}
			int marked = s != nullptr ? colour_count(s, {settings_card_x + 8,
				int(settings_agents_switch_y) - 18, settings_label_w, 36},
				attention_dark) : 0;
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			if (auto wait = wait_for(!((runs.size() != settings_base_runs || !agents_off || others_on != int(widgets.size()) - 1 || marked < 10)), {.poll_ms = 700, .budget_ms = 21000})) {
				shot("settings-bar-hidden");
				return *wait;
			}
			check(runs.size() == settings_base_runs,
				std::format("the bar lost the agents widget without a reload "
					"({} runs, was {})", runs.size(), settings_base_runs + 1));
			check(agents_off && others_on == int(widgets.size()) - 1,
				std::format("the agents switch shows off and the other {} stay on",
					widgets.size() - 1));
			check(marked >= 10, std::format("the hidden widget's label is marked "
				"in the attention colour ({} pixels)", marked));
			// The right section is anchored at its edge, so the neighbours
			// slide into the vacated place at once: which widget covers that
			// x now says nothing. The run count and the same bar surface
			// above are the proof that the hide took and nothing reloaded.
			check(settings_shell_kept(),
				"the same bar surface lost it: the shell was not reloaded");
			return after(500);
		}},
		{"settings_switch_again", [] {
			click(settings_card_x + settings_pad
				+ settings_panel - 2 * settings_pad - 8 - settings_switch_w / 2.0,
				settings_agents_switch_y);
			return after(400);
		}},
		{"settings_bar_hidden_empty_again_the", [] {
			std::vector<std::string> hidden = shell_bar_hidden();
			if (auto wait = wait_for(hidden.empty(), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(hidden.empty(), "showing the widget emptied bar.hidden in shell.json");
			shot("settings-bar-shown");
			return after(500);
		}},
		{"settings_bar_regains_the_run_still", [] {
			cairo_surface_t *s = load_png("settings-bar-shown");
			LayerSurface *bar = bar_layer();
			std::vector<Run> runs = s != nullptr && bar != nullptr
				? bar_runs(s, bar->geo) : std::vector<Run>{};
			std::vector<double> widgets = settings_bar_rows(settings_card_y);
			int on = 0;
			for (double y : widgets) {
				on += s != nullptr && settings_switch_is(s, y, true);
			}
			int marked = s != nullptr ? colour_count(s, {settings_card_x + 8,
				int(settings_agents_switch_y) - 18, settings_label_w, 36},
				attention_dark) : 0;
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			if (auto wait = wait_for(!((runs.size() != settings_base_runs + 1 || on != int(widgets.size()) || marked >= 10)), {.poll_ms = 700, .budget_ms = 21000})) {
				shot("settings-bar-shown");
				return *wait;
			}
			check(runs.size() == settings_base_runs + 1,
				std::format("the bar regained the agents widget ({} runs)",
					runs.size()));
			check(on == int(widgets.size()), std::format("every widget's switch "
				"is on again ({} of {})", on, widgets.size()));
			check(marked < 10, "the agents row's label is back to the normal colour");
			bool covered = false;
			for (const Run &run : runs) {
				covered = covered || (agents_x >= run.x0 && agents_x <= run.x1);
			}
			check(covered, "the agents widget is back where it sat");
			return after(500);
		}},
		{"settings_toolbar_tab", [] {
			// The card still wears the Bar tab's shape, so its tabs row sits
			// where that card puts it; the shorter shape only re-centres the
			// card once the click lands.
			{
				auto [tx, ty] = settings_tab_point(3, settings_card_y); // Toolbar
				click(tx, ty);
			}
			return after(700);
		}},
		{"settings_card_redraws_short", [] {
			shot("settings-toolbar-tab");
			return after(700);
		}},
		{"settings_four_edges_listed", [] {
			cairo_surface_t *s = load_png("settings-toolbar-tab");
			int card_y = settings_card_top(
				settings_card_height(settings_toolbar_column()), settings_gear_y);
			int first_top = card_y + settings_pad + settings_tabs_h + settings_gap
				+ settings_header + settings_gap;
			// Labels only: the dots would light a row by themselves.
			std::vector<Band> rows = bands_in(s,
				{settings_card_x + 8, card_y, settings_label_w,
					settings_card_height(settings_toolbar_column())});
			int chosen = s != nullptr ? settings_chosen_tab(s, card_y) : -1;
			unsigned dots = s != nullptr ? settings_dots(s, first_top, 4) : 0;
			cairo_surface_destroy(s);
			int labelled = settings_choices_labelled(rows, first_top, 4);
			if (auto wait = wait_for(!((chosen != 3 || labelled != 4 || dots != 1u)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-toolbar-tab");
				return *wait;
			}
			check(chosen == 3, "the Toolbar tab is the one chosen");
			check(labelled == 4, std::format("the Toolbar tab lists the four "
				"edges, a label on each ({} of 4)", labelled));
			check(dots == 1u, "Right, the default edge, is the only one chosen "
				"before any click");
			// The second choice, Left: the first choice's top is a header and
			// a gap below the tabs.
			click(settings_card_x + 100, first_top + settings_choice
				+ settings_gap + settings_choice / 2.0);
			return after(400);
		}},
		{"settings_write_2", [] {
			std::string position = shell_toolbar_position();
			bool moved = toolbar_on_edge("left");
			if (auto wait = wait_for(!((position != "left" || !moved)), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(position == "left",
				"choosing Left wrote toolbar.position into shell.json");
			check(moved, "the strip moved to the left edge with the choice, 8 px "
				"from it");
			check(settings_shell_kept(), "the strip moved to the left with no "
				"reload of the shell");
			shot("settings-toolbar-left");
			return after(500);
		}},
		{"settings_left_is_chosen_and_the", [] {
			ToolbarCard card = settings_toolbar_card("settings-toolbar-left");
			if (auto wait = wait_for(!((card.dots != 2u || !card.placed)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-toolbar-left");
				return *wait;
			}
			check(card.dots == 2u, "choosing Left lit its dot and only its dot: "
				"the choice shows as the selected edge");
			check(card.placed, std::format("the settings card moved with the "
				"strip: beside it on the left edge, level with the gear (card at "
				"{} px)", settings_card_x));
			// The third choice, Top.
			click(settings_card_x + 100, card.first_top
				+ 2 * (settings_choice + settings_gap) + settings_choice / 2.0);
			return after(400);
		}},
		{"settings_write_3", [] {
			std::string position = shell_toolbar_position();
			bool moved = toolbar_on_edge("top");
			if (auto wait = wait_for(!((position != "top" || !moved)), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(position == "top",
				"choosing Top wrote toolbar.position into shell.json");
			check(moved, "the strip moved to the top edge with the choice: a row "
				"below the bar, 8 px down");
			check(settings_shell_kept(), "the strip moved to the top with no "
				"reload of the shell");
			shot("settings-toolbar-top");
			return after(500);
		}},
		{"settings_top_is_chosen_and_the", [] {
			ToolbarCard card = settings_toolbar_card("settings-toolbar-top");
			if (auto wait = wait_for(!((card.dots != 4u || !card.placed)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-toolbar-top");
				return *wait;
			}
			LayerSurface *toolbar = toolbar_layer();
			wlr_box strip = toolbar != nullptr ? toolbar_strip_box(toolbar->geo) : wlr_box{};
			check(card.dots == 4u, "choosing Top lit its dot and only its dot");
			check(card.placed && card.card_y == strip.y + strip.height + beside_gap,
				std::format("the settings card hangs one margin below the top row, "
					"centred on the gear (card at {},{}; the row's bottom at {})",
					settings_card_x, card.card_y, strip.y + strip.height));
			click(settings_card_x + 100, card.first_top + settings_choice / 2.0);
			return after(400);
		}},
		{"settings_write_4", [] {
			std::string position = shell_toolbar_position();
			bool moved = toolbar_on_edge("right");
			if (auto wait = wait_for(!((position != "right" || !moved)), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(position == "right",
				"choosing Right wrote toolbar.position back");
			check(moved, "the strip moved back to the right edge with the choice");
			shot("settings-toolbar-right");
			return after(500);
		}},
		{"settings_right_is_the_chosen_edge", [] {
			ToolbarCard card = settings_toolbar_card("settings-toolbar-right");
			if (auto wait = wait_for(!((card.dots != 1u || !card.placed)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-toolbar-right");
				return *wait;
			}
			check(card.dots == 1u, "Right is the only edge chosen again");
			check(card.placed && settings_card_x == settings_card_left(false),
				std::format("the settings card is back beside the right edge, "
					"level with the gear (card at {} px)", settings_card_x));
			check(settings_shell_kept(), "choosing the toolbar's edge three times "
				"wrote shell.json and reloaded nothing");
			// The card keeps the Toolbar tab's shape until the click lands.
			{
				auto [tx, ty] = settings_tab_point(0, card.card_y); // Shortcuts
				click(tx, ty);
			}
			return after(700);
		}},
		{"settings_shortcuts_tab_drawn", [] {
			shot("settings-shortcuts-tab");
			return after(700);
		}},
		{"settings_shortcuts_tab", [] {
			cairo_surface_t *s = load_png("settings-shortcuts-tab");
			// The tall card clamps at the top; the terminal row is the second
			// of the Apps group, under its header and the launcher row.
			int terminal_y = 8 + settings_pad + settings_tabs_h + settings_gap
				+ settings_header + settings_gap + settings_action + settings_gap
				+ settings_row / 2;
			int terminal_top = terminal_y - 20;
			std::vector<Band> rows = bands_in(s,
				{settings_card_x + 8, terminal_top, settings_panel - 16, 40});
			int chosen = s != nullptr ? settings_chosen_tab(s, 8) : -1;
			// The Terminal row's chords line: its chip, from the row's left edge.
			settings_terminal_y = terminal_y;
			int chip = s != nullptr
				? settings_chip_width(s, terminal_y + settings_row - 7) : 0;
			cairo_surface_destroy(s);
			auto terminal = settings_band_at(rows, terminal_y);
			bool listed = terminal != rows.end()
				&& std::abs(terminal->centre() - terminal_y) <= 12;
			if (auto wait = wait_for(!((!listed || chosen != 0 || chip < 20)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-shortcuts-tab");
				return *wait;
			}
			check(chosen == 0, "the Shortcuts tab is the one chosen");
			check(listed, "the Shortcuts tab lists the actions (the Apps group's rows)");
			check(chip >= 20, std::format("the Terminal row's chord is drawn as a "
				"key chip ({} px wide)", chip));
			settings_chip_before = chip;
			click(settings_card_x + 30, terminal_y);
			return after(400);
		}},
		{"settings_capture_takes_every_key", [] {
			chord(KEY_T, mods::super); // Super+T, the terminal's new chord
			return after(400);
		}},
		{"settings_canvas_conf_gains_the_line", [] {
			std::string keys = conf_value("terminal");
			// Tiling held Super+T by default, with no line of its own to
			// rewrite: the running session says whether it lost it.
			bool taken = holds_chord("terminal", {settings::mod_logo, XKB_KEY_t})
				&& !holds_chord("tiling", {settings::mod_logo, XKB_KEY_t});
			if (auto wait = wait_for(!((keys != "Super+T" || !taken)), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(keys == "Super+T",
				"recording Super+T wrote terminal = Super+T into canvas.conf");
			check(conf_count("Super+T") == 1,
				std::format("no other line of canvas.conf holds Super+T ({} "
					"Super+T in the file)", conf_count("Super+T")));
			check(taken, "the running session moved Super+T from Tiling to "
				"Terminal");
			check(panel_layer("settings") != nullptr, "the panel kept open "
				"through the recording");
			check(settings_shell_kept(), "the recording rewrote canvas.conf and "
				"the shell was not reloaded");
			return after(0);
		}},
		{"settings_chip_shrank", [] {
			double note_y = settings_terminal_y + 1.5 * settings_row
				+ settings_detail / 2.0; // under the chords: where the chord came from
			auto has_note = [&](cairo_surface_t *s) {
				std::vector<Band> lines = bands_in(s,
					{settings_card_x + 8, settings_terminal_y - 20, settings_label_w,
						3 * settings_row});
				auto note = settings_band_at(lines, note_y);
				return note != lines.end() && std::abs(note->centre() - note_y) <= 8;
			};
			auto chip_of = [&](cairo_surface_t *s) {
				return settings_chip_width(s, settings_terminal_y + settings_row - 7);
			};
			if (auto wait = shot_holds("settings-shortcuts-set", [&](cairo_surface_t *s) {
					int shown = chip_of(s);
					return has_note(s) && shown > 0 && shown < settings_chip_before;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("settings-shortcuts-set");
			int chip = s != nullptr ? chip_of(s) : 0;
			bool noted = s != nullptr && has_note(s);
			cairo_surface_destroy(s);
			check(noted, "the Terminal row grew a line under its chord: where "
				"Super+T was taken from");
			check(chip > 0 && chip < settings_chip_before, std::format("the "
				"Terminal row's chip shows the shorter Super+T ({} px, was {})",
				chip, settings_chip_before));
			// The card is the tall Shortcuts shape: the tabs at its top; the
			// click shrinks it to the presets, so the next step waits first.
			{
				auto [tx, ty] = settings_tab_point(1, 8); // Gestures
				click(tx, ty);
			}
			return after(700);
		}},
		{"settings_card_redraws_to_the_presets", [] {
			shot("settings-gestures-tab");
			return after(700);
		}},
		{"settings_choices_drawn", [] {
			cairo_surface_t *s = load_png("settings-gestures-tab");
			int column = settings_gestures_column();
			int card_y = settings_card_top(settings_card_height(column),
				settings_gear_y);
			int first_top = card_y + settings_pad + settings_tabs_h + settings_gap
				+ settings_header + settings_gap;
			// The pinch row: under the second header, a detailed row.
			double pinch_y = first_top + 3 * (settings_choice + settings_gap)
				+ settings_header + settings_gap + settings_choice / 2.0;
			// The direction rows: one header further down again, five detailed
			// rows, their switches at the defaults: natural, natural, natural,
			// up and toward.
			double direction_y = pinch_y + settings_choice + settings_header
				+ 2 * settings_gap;
			constexpr std::array<bool, 5> direction_on = {true, true, true, false, false};
			// Labels only: the dots and the switch would light a row by themselves.
			std::vector<Band> rows = bands_in(s,
				{settings_card_x + 8, card_y, settings_label_w,
					settings_card_height(column)});
			int chosen = s != nullptr ? settings_chosen_tab(s, card_y) : -1;
			unsigned dots = s != nullptr ? settings_dots(s, first_top, 3) : 0;
			bool pinch_on = s != nullptr && settings_switch_is(s, pinch_y, true);
			int directions_right = 0;
			for (std::size_t k = 0; k < direction_on.size(); k++) {
				double y = direction_y + static_cast<double>(k) * (settings_choice + settings_gap);
				auto band = settings_band_at(rows, y);
				bool labelled = band != rows.end() && std::abs(band->centre() - y) <= 12;
				if (labelled && s != nullptr && settings_switch_is(s, y, direction_on[k])) {
					directions_right++;
				}
			}
			cairo_surface_destroy(s);
			int labelled = settings_choices_labelled(rows, first_top, 3);
			auto pinch = settings_band_at(rows, pinch_y);
			bool pinch_labelled = pinch != rows.end()
				&& std::abs(pinch->centre() - pinch_y) <= 12;
			if (auto wait = wait_for(!((chosen != 1 || labelled != 3 || !pinch_labelled || dots != 1u || !pinch_on || directions_right != 5)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-gestures-tab");
				return *wait;
			}
			check(chosen == 1, "the Gestures tab is the one chosen");
			check(labelled == 3 && pinch_labelled, std::format("the Gestures tab "
				"lists the three presets and the pinch row, a label on each "
				"({} of 3 presets)", labelled));
			check(dots == 1u, "Three-finger hops, the default, is the only "
				"preset chosen before any click");
			check(pinch_on, "the pinch switch shows on, its default");
			check(directions_right == 5, std::format("the Gestures tab ends with the five "
				"direction rows, each labelled and its switch at its default ({} of 5)",
				directions_right));
			settings_first_top = first_top;
			click(settings_card_x + 100, first_top + settings_choice
				+ settings_gap + settings_choice / 2.0);
			return after(400);
		}},
		{"settings_canvas_conf_says_which_preset", [] {
			std::string swipes = conf_value("swipes");
			if (auto wait = wait_for(swipes == "four-finger-hops", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(swipes == "four-finger-hops",
				"choosing Four-finger hops wrote swipes = four-finger-hops "
				"into canvas.conf");
			shot("settings-gestures-set");
			return after(700);
		}},
		{"settings_canvas_confs_directory_refuses_the", [] {
			std::string dir = canvas_conf.substr(0, canvas_conf.rfind('/'));
			settings_conf_before = read_file(canvas_conf);
			chmod(dir.c_str(), 0555);
			click(settings_card_x + settings_pad + settings_panel - 2 * settings_pad
				- 8 - settings_switch_w / 2.0, settings_direction_y(4));
			return after(500);
		}},
		{"settings_shot_settings_gestures_refused", [] {
			shot("settings-gestures-refused");
			return after(700);
		}},
		{"settings_refusal_is_on_the_row", [] {
			cairo_surface_t *s = load_png("settings-gestures-refused");
			double y = settings_direction_y(4);
			int marked = s != nullptr ? colour_count(s, {settings_card_x + 8,
				int(y) - 18, settings_label_w, 36}, attention_dark) : 0;
			cairo_surface_destroy(s);
			if (auto wait = wait_for(marked >= 10, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-gestures-refused");
				return *wait;
			}
			check(marked >= 10, std::format("a write canvas.conf's directory "
				"refused shows on its own row, the hop row's label in the "
				"attention colour ({} px)", marked));
			check(read_file(canvas_conf) == settings_conf_before, "the refused "
				"write left canvas.conf as it was");
			std::string dir = canvas_conf.substr(0, canvas_conf.rfind('/'));
			chmod(dir.c_str(), 0755);
			click(settings_card_x + settings_pad + settings_panel - 2 * settings_pad
				- 8 - settings_switch_w / 2.0, y);
			return after(400);
		}},
		{"settings_same_click_the_directory_writable", [] {
			std::string hop = conf_value("hop-swipe");
			if (auto wait = wait_for(hop == "away", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(hop == "away", "the next click wrote hop-swipe = away into "
				"canvas.conf");
			shot("settings-gestures-written");
			return after(700);
		}},
		{"settings_that_write_cleared_the_rows", [] {
			cairo_surface_t *s = load_png("settings-gestures-written");
			double y = settings_direction_y(4);
			int marked = s != nullptr ? colour_count(s, {settings_card_x + 8,
				int(y) - 18, settings_label_w, 36}, attention_dark) : 0;
			bool on = s != nullptr && settings_switch_is(s, y, true);
			cairo_surface_destroy(s);
			if (auto wait = wait_for(!((marked >= 10 || !on)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-gestures-written");
				return *wait;
			}
			check(marked < 10 && on, std::format("the write that went through "
				"cleared the row's fault and its switch shows on ({} attention "
				"px)", marked));
			click(settings_card_x + settings_pad + settings_panel - 2 * settings_pad
				- 8 - settings_switch_w / 2.0, y);
			return after(400);
		}},
		{"settings_switch_put_back_where_it", [] {
			std::string hop = conf_value("hop-swipe");
			if (auto wait = wait_for(hop == "toward", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(hop == "toward", "the second click wrote hop-swipe = toward");
			return after(700);
		}},
		{"settings_choice_drawn_the_panel_up", [] {
			if (!settings_esc_sent) {
				cairo_surface_t *s = load_png("settings-gestures-set");
				unsigned dots = s != nullptr ? settings_dots(s, settings_first_top, 3) : 0;
				cairo_surface_destroy(s);
				if (auto wait = wait_for(dots == 2u, {.poll_ms = 700, .budget_ms = 15000})) {
					shot("settings-gestures-set");
					return *wait;
				}
				check(dots == 2u, "Four-finger hops is the only preset drawn chosen");
				check(panel_layer("settings") != nullptr && settings_shell_kept(),
					"the compositor's canvas.conf write left the panel open and "
					"the shell unreloaded");
				settings_esc_sent = true;
				tap(KEY_ESC);
				return again(400);
			}
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the settings panel closed on Escape");
			views_before = mapped_views();
			terminals_before = mapped_views_of("foot");
			chord(KEY_T, mods::super); // the terminal's new chord, in the overview
			return after(400);
		}},
		{"settings_terminal_opens", [] {
			bool opened = mapped_views() == views_before + 1
				&& mapped_views_of("foot") == terminals_before + 1;
			if (auto wait = wait_for(opened, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(opened, "the recorded chord opened the terminal (one new foot window)");
			chord(KEY_ENTER, mods::super); // the chord it replaced
			return after(1500); // a foot window needs a moment: the wait must outlast one
		}},
		{"settings_and_the_replaced_chord_opens", [] {
			check(mapped_views() == views_before + 1
					&& mapped_views_of("foot") == terminals_before + 1,
				"the chord the recording replaced opens nothing");
			return after(400);
		}},
		{"settings_panel_again_to_record_a", [] {
			check(toolbar_layer() != nullptr && panel_layers() == 0,
				"the overview still shows its toolbar, with no panel open");
			click(strip_left + toolbar_button / 2.0, settings_gear_y);
			return after(400);
		}},
		{"settings_panel_is_back_on_the", [] {
			if (auto wait = wait_for(panel_layer("settings") != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("settings") != nullptr,
				"the gear opened the settings panel a second time");
			shot("settings-again");
			return after(700);
		}},
		{"settings_gestures_tab_as_it_was", [] {
			int column = settings_gestures_column();
			int card_y = settings_card_top(settings_card_height(column), settings_gear_y);
			cairo_surface_t *s = load_png("settings-again");
			int chosen = s != nullptr ? settings_chosen_tab(s, card_y) : -1;
			cairo_surface_destroy(s);
			if (auto wait = wait_for(chosen >= 0, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-again");
				return *wait;
			}
			if (chosen != 0) {
				auto [tx, ty] = settings_tab_point(0, card_y); // Shortcuts
				click(tx, ty);
			}
			return after(700);
		}},
		{"settings_tall_shortcuts_card_redraws", [] {
			shot("settings-shortcuts-again");
			return after(700);
		}},
		{"settings_app_launcher_row_which_has", [] {
			cairo_surface_t *s = load_png("settings-shortcuts-again");
			int chosen = s != nullptr ? settings_chosen_tab(s, 8) : -1;
			std::vector<Band> rows = s != nullptr ? bands_in(s,
				{settings_card_x + 8, settings_launcher_y() - 20, settings_panel - 16, 40})
				: std::vector<Band>{};
			cairo_surface_destroy(s);
			auto launcher = settings_band_at(rows, settings_launcher_y());
			bool listed = launcher != rows.end()
				&& std::abs(launcher->centre() - settings_launcher_y()) <= 12;
			if (auto wait = wait_for(!((chosen != 0 || !listed)), {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-shortcuts-again");
				return *wait;
			}
			check(chosen == 0 && listed, "the Shortcuts tab is up again with the "
				"App launcher row at its place");
			click(settings_card_x + 30, settings_launcher_y());
			return after(400);
		}},
		{"settings_shot_settings_recording", [] {
			shot("settings-recording");
			return after(700);
		}},
		{"settings_row_says_it_records", [] {
			bool shown = settings_prompt_shown("settings-recording");
			if (auto wait = wait_for(shown, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-recording");
				return *wait;
			}
			check(shown, "the App launcher row says it records (the prompt line under its chords)");
			tap(KEY_ESC);
			return after(400);
		}},
		{"settings_shot_settings_escaped", [] {
			shot("settings-escaped");
			return after(700);
		}},
		{"settings_capture_took_the_escape", [] {
			bool shown = settings_prompt_shown("settings-escaped");
			if (auto wait = wait_for(!shown, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-escaped");
				return *wait;
			}
			check(!shown, "Escape alone cancelled the recording: the prompt is gone");
			check(panel_layer("settings") != nullptr,
				"and the panel stayed open: the capture, not the panel, took the Escape");
			return after(400);
			return after(400);
		}},
		{"settings_reset_on_the_changed_terminal", [] {
			click(settings_card_x + settings_panel - 80, settings_terminal_y);
			return after(400);
		}},
		{"settings_canvas_conf_holds_the_defaults", [] {
			std::string keys = conf_value("terminal");
			if (auto wait = wait_for(keys == "Super+Return", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(keys == "Super+Return", std::format("Reset put the Terminal row's "
				"defaults back into canvas.conf (terminal = \"{}\")", keys));
			check(holds_chord("terminal", {settings::mod_logo, XKB_KEY_Return}),
				"the running session has Super+Return on Terminal again");
			views_before = mapped_views();
			terminals_before = mapped_views_of("foot");
			chord(KEY_ENTER, mods::super);
			return after(1500); // a foot window needs a moment: the wait must outlast one
		}},
		{"settings_default_chord_opened_a_terminal", [] {
			check(mapped_views() == views_before + 1
					&& mapped_views_of("foot") == terminals_before + 1,
				"the default chord works (Super+Return opened one new foot window)");
			click(settings_card_x + settings_panel - 36, settings_terminal_y);
			return after(400);
		}},
		{"settings_canvas_conf_says_none", [] {
			std::string keys = conf_value("terminal");
			if (auto wait = wait_for(keys == "none", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(keys == "none", std::format("Clear wrote terminal = none into "
				"canvas.conf (\"{}\")", keys));
			check(!holds_chord("terminal", {settings::mod_logo, XKB_KEY_Return}),
				"the running session left Terminal without its chord");
			views_before = mapped_views();
			terminals_before = mapped_views_of("foot");
			chord(KEY_ENTER, mods::super);
			return after(1500);
		}},
		{"settings_old_chord_opened_nothing", [] {
			check(mapped_views() == views_before
					&& mapped_views_of("foot") == terminals_before,
				"the cleared action's old chord opens nothing");
			click(settings_card_x + settings_panel - 36, settings_terminal_y);
			return after(400);
		}},
		{"settings_restored", [] {
			std::string keys = conf_value("terminal");
			if (auto wait = wait_for(keys == "Super+Return", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(keys == "Super+Return", std::format("Reset after Clear put terminal "
				"= Super+Return back (\"{}\")", keys));
			// The command the new row will run: a script on the PATH the
			// compositor's commands inherit, touching a marker the scenario sees.
			command_marker = shot_dir + "/command-marker";
			unlink(command_marker.c_str());
			std::string script = shot_dir + "/tool-bin/markcmd";
			write_file(script, std::format("#!/bin/sh\ntouch '{}'\n", command_marker));
			chmod(script.c_str(), 0755);
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, 150);
			shot("settings-commands-end");
			return after(700);
		}},
		{"settings_at_the_lists_end_the", [] {
			// At its end when one more wheel burst moved nothing: two shots alike.
			bool settled = settings_png_repeats("settings-commands-end");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				scroll(settings_card_x + 100, 400, 150);
				shot("settings-commands-end");
				return *wait;
			}
			cairo_surface_t *s = load_png("settings-commands-end");
			std::vector<Band> rows = s != nullptr ? bands_in(s,
				{settings_card_x + 8, settings_list_bottom - 3 * settings_row,
					settings_label_w, 3 * settings_row}) : std::vector<Band>{};
			cairo_surface_destroy(s);
			auto add = settings_band_at(rows, settings_last_row_y);
			bool listed = add != rows.end()
				&& std::abs(add->centre() - settings_last_row_y) <= 12;
			check(settled && listed, "scrolled to its end (a further wheel burst moved "
				"nothing), the Shortcuts list shows the Add a command row at its bottom");
			click(settings_card_x + 60, settings_last_row_y);
			return after(400);
		}},
		{"settings_draft_row_stands_above_the", [] {
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, 40);
			shot("settings-draft");
			return after(700);
		}},
		{"settings_draft_rows_chord_chip_is", [] {
			bool settled = settings_png_repeats("settings-draft");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-draft");
				return *wait;
			}
			cairo_surface_t *s = load_png("settings-draft");
			// Along a line under the chip's text, which would end the run.
			int chip = s != nullptr
				? settings_chip_width(s, int(settings_command_y) + 7) : 0;
			cairo_surface_destroy(s);
			check(settled && chip >= 20, std::format("the Add row opened a draft "
				"command row above itself, with a chord chip ({} px wide)", chip));
			settings_draft_chip = chip;
			click(settings_card_x + settings_pad + 8 + 12, settings_command_y);
			return after(400);
		}},
		{"settings_capture_takes_every_key_2", [] {
			chord(KEY_Y, mods::super);
			return after(1000); // long enough for a write, and the error it would bring, to land
		}},
		{"settings_draft_holds_the_chord", [] {
			check(read_file(canvas_conf).find("Super+Y") == std::string::npos,
				"recording the draft's chord wrote nothing into canvas.conf");
			check(server->conf.errors().empty()
					&& !server->hint.text().starts_with("canvas.conf line"),
				"and no file error followed it (none in the settings, no hint)");
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, 40);
			shot("settings-recorded");
			return after(700);
		}},
		{"settings_row_shows_the_chord_it", [] {
			bool settled = settings_png_repeats("settings-recorded");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-recorded");
				return *wait;
			}
			cairo_surface_t *s = load_png("settings-recorded");
			int chip = s != nullptr
				? settings_chip_width(s, int(settings_command_y) + 7) : 0;
			cairo_surface_destroy(s);
			check(settled && chip > settings_draft_chip, std::format("the draft row "
				"shows the chord it holds, in a chip wider than None ({} px, was {})",
				chip, settings_draft_chip));
			click(settings_card_x + 180, settings_command_y);
			return after(500);
		}},
		{"settings_field_has_the_keyboard", [] {
			LayerSurface *panel = panel_layer("settings");
			check(panel != nullptr
					&& server->seat->keyboard_state.focused_surface == panel->wlr->surface,
				"the settings panel holds the keyboard for the command field");
			check(read_file(canvas_conf).find("Super+Y") == std::string::npos,
				"the chord alone is still not in canvas.conf");
			type_text("markcmd");
			shot("settings-command-typed");
			return after(500);
		}},
		{"settings_enter_saves_the_line", [] {
			tap(KEY_ENTER);
			return after(400);
		}},
		{"settings_canvas_conf_holds_the_command", [] {
			std::string command = conf_value("Super+Y");
			if (auto wait = wait_for(command == "markcmd", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(command == "markcmd", std::format("Enter on the typed command line "
				"wrote Super+Y = markcmd into [commands] (\"{}\")", command));
			check(server->conf.errors().empty(), "and the file has no error");
			check(!std::filesystem::exists(command_marker),
				"the command had not run before its chord was pressed");
			chord(KEY_Y, mods::super);
			return after(400);
		}},
		{"settings_marker_is_there", [] {
			if (auto wait = wait_for(std::filesystem::exists(command_marker), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(std::filesystem::exists(command_marker),
				"the new command's chord ran it (its marker file appeared)");
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, 40);
			shot("settings-saved");
			return after(700);
		}},
		{"settings_saved_rows_command_field_is", [] {
			bool settled = settings_png_repeats("settings-saved");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-saved");
				return *wait;
			}
			check(settled, "the list stopped moving after the save");
			click(settings_card_x + 180, settings_command_y);
			return after(500);
		}},
		{"settings_select_all_delete_enter", [] {
			chord(KEY_A, mods::ctrl);
			tap(KEY_BACKSPACE);
			tap(KEY_ENTER);
			return after(1000);
		}},
		{"settings_saved_nothing", [] {
			check(conf_value("Super+Y") == "markcmd" && server->conf.errors().empty(),
				"an emptied command field saved nothing: the line still holds markcmd");
			click(settings_card_x + settings_panel - 40, settings_command_y);
			return after(400);
		}},
		{"settings_remove_took_the_line_out", [] {
			std::string text = read_file(canvas_conf);
			if (auto wait = wait_for(text.find("Super+Y") == std::string::npos, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(text.find("Super+Y") == std::string::npos,
				"Remove took the command's line out of canvas.conf");
			check(server->conf.resolved.commands.empty(),
				"the running session has no command left");
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, 40);
			shot("settings-removed");
			return after(700);
		}},
		{"settings_list_at_its_end_the", [] {
			bool settled = settings_png_repeats("settings-removed");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-removed");
				return *wait;
			}
			check(settled, "the list stopped moving after the Remove");
			click(settings_card_x + 60, settings_last_row_y);
			return after(400);
		}},
		{"settings_second_draft_row_stands_above", [] {
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, 40);
			shot("settings-draft-two");
			return after(700);
		}},
		{"settings_its_command_field_is_clicked", [] {
			bool settled = settings_png_repeats("settings-draft-two");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-draft-two");
				return *wait;
			}
			check(settled, "the list stopped moving after the second Add");
			click(settings_card_x + 180, settings_command_y);
			return after(500);
		}},
		{"settings_command_line_typed_no_enter", [] {
			unlink(command_marker.c_str());
			type_text("markcmd");
			shot("settings-text-first");
			return after(500);
		}},
		{"settings_typing_wrote_nothing", [] {
			check(read_file(canvas_conf).find("Super+U =") == std::string::npos
					&& server->conf.errors().empty(),
				"the typed command line alone wrote nothing into canvas.conf");
			click(settings_card_x + settings_pad + 8 + 12, settings_command_y);
			return after(400);
		}},
		{"settings_capture_takes_every_key_3", [] {
			chord(KEY_U, mods::super);
			return after(1000);
		}},
		{"settings_typed_text_rode_along", [] {
			std::string text = read_file(canvas_conf);
			std::string command = conf_value("Super+U");
			if (auto wait = wait_for(command == "markcmd", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			std::size_t lines = 0;
			for (std::size_t at = text.find("Super+U ="); at != std::string::npos;
					at = text.find("Super+U =", at + 1)) {
				lines++;
			}
			check(command == "markcmd" && lines == 1, std::format("recording the "
				"chord after typing, without Enter, wrote Super+U = markcmd once "
				"(\"{}\", {} line(s))", command, lines));
			check(server->conf.errors().empty()
					&& !server->hint.text().starts_with("canvas.conf line"),
				"and no file error followed it");
			check(!std::filesystem::exists(command_marker),
				"the command had not run before its chord was pressed");
			chord(KEY_U, mods::super);
			return after(400);
		}},
		{"settings_marker_is_there_2", [] {
			if (auto wait = wait_for(std::filesystem::exists(command_marker), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(std::filesystem::exists(command_marker),
				"the chord recorded after typing runs the command (its marker appeared)");
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, 40);
			shot("settings-text-saved");
			return after(700);
		}},
		{"settings_remove_on_the_row", [] {
			bool settled = settings_png_repeats("settings-text-saved");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-text-saved");
				return *wait;
			}
			check(settled, "the list stopped moving after the save");
			click(settings_card_x + settings_panel - 40, settings_command_y);
			return after(400);
		}},
		{"settings_remove_took_the_line_out_2", [] {
			std::string text = read_file(canvas_conf);
			if (auto wait = wait_for(text.find("Super+U =") == std::string::npos, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(text.find("Super+U =") == std::string::npos,
				"Remove took the second command's line out of canvas.conf");
			check(server->conf.resolved.commands.empty() && server->conf.errors().empty(),
				"the running session has no command left, and no file error");
			settings_last_png.clear();
			scroll(settings_card_x + 100, 400, -150);
			shot("settings-top-again");
			return after(700);
		}},
		{"settings_list_back_at_its_top", [] {
			bool settled = settings_png_repeats("settings-top-again");
			if (auto wait = wait_for(settled, {.poll_ms = 700, .budget_ms = 15000})) {
				scroll(settings_card_x + 100, 400, -150);
				shot("settings-top-again");
				return *wait;
			}
			cairo_surface_t *s = load_png("settings-top-again");
			std::vector<Band> rows = s != nullptr ? bands_in(s,
				{settings_card_x + 8, settings_launcher_y() - 20, settings_label_w, 40})
				: std::vector<Band>{};
			cairo_surface_destroy(s);
			auto launcher = settings_band_at(rows, settings_launcher_y());
			bool listed = launcher != rows.end()
				&& std::abs(launcher->centre() - settings_launcher_y()) <= 12;
			check(settled && listed, "scrolled back to its top (a further wheel burst "
				"moved nothing), the list shows the App launcher row");
			click(settings_card_x + 30, settings_launcher_y());
			return after(400);
		}},
		{"settings_shot_settings_recording_again", [] {
			shot("settings-recording-again");
			return after(700);
		}},
		{"settings_recording_again", [] {
			bool shown = settings_prompt_shown("settings-recording-again");
			if (auto wait = wait_for(shown, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("settings-recording-again");
				return *wait;
			}
			check(shown, "the App launcher row records a second time");
			click(strip_left + toolbar_button / 2.0, settings_gear_y);
			return after(400);
		}},
		{"settings_closed_while_recording", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0,
				"the gear's second click closed the panel while a row was recording");
			// The terminal this chord opens writes what it is typed into a
			// marker, so a plain key reaching it is observable.
			keys_marker = shot_dir + "/keys-marker";
			unlink(keys_marker.c_str());
			std::string script = shot_dir + "/keys-terminal.sh";
			write_file(script, std::format(
				"#!/bin/sh\nexec foot -T keys sh -c 'cat > {}'\n", keys_marker));
			chmod(script.c_str(), 0755);
			setenv("TERMINAL", script.c_str(), 1);
			views_before = mapped_views();
			terminals_before = mapped_views_of("foot");
			// The first key after the close: a recording left alive would take it.
			chord(KEY_ENTER, mods::super);
			return after(400);
		}},
		{"settings_chord_ran_its_action_again", [] {
			bool opened = mapped_views() == views_before + 1
				&& mapped_views_of("foot") == terminals_before + 1;
			if (auto wait = wait_for(opened, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(opened, "after the close, the chord ran its action again "
				"(Super+Return opened one new foot window)");
			tap(KEY_ESC); // out of the overview
			return after(400);
		}},
		{"settings_out_of_the_overview_the", [] {
			View *keys = titled("keys");
			bool ready = overview_settled(false) && toolbar_layer() == nullptr
				&& keys != nullptr && server->focus.focused_view() == keys;
			if (auto wait = wait_for(ready, {.poll_ms = 400, .budget_ms = 60000})) {
				if (polls() == 12 && keys != nullptr && server->focus.focused_view() != keys) {
					wlr_box f = keys->frame_box();
					click(f.x + f.width / 2.0, f.y + f.height / 2.0);
				}
				return *wait;
			}
			check(overview_settled(false), "Escape left the overview: no recording swallowed it");
			check(keys != nullptr && server->focus.focused_view() == keys,
				"the terminal the chord opened is the focused client");
			tap(KEY_Q); // one plain key, then Enter for the terminal's line discipline
			tap(KEY_ENTER);
			return after(600);
		}},
		{"settings_plain_key_reached_the_client", [] {
			std::string typed = read_file(keys_marker);
			if (auto wait = wait_for(typed.find('q') != std::string::npos, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(typed.find('q') != std::string::npos,
				"after the close, a plain key reached the focused client");
			check(conf_value("terminal") == "Super+Return",
				"and no recording took it: terminal is still Super+Return in canvas.conf");
			return after(0);
		}},
	};
}

} // namespace bar
