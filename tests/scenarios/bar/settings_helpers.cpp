// The settings window's geometry mirrored off its files, and the shell's own config reads.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr int settings_sep = 1;

// What the panel's own fills look like on a screenshot (dark theme).
constexpr uint32_t selected_dark = 0x2f3954ff; // UiTheme.selected over the panel: the accent at 22 %
constexpr uint32_t track_dark = 0x3a3b45ff;    // UiTheme.track over the panel: a switch that is off

// shell.json's `bar` object, or null: the hidden list and the toolbar's
// edge are read from it.
JsonObject *shell_json_bar(JsonParser *parser) {
	if (!json_parser_load_from_file(parser, shell_config.c_str(), nullptr)) {
		return nullptr;
	}
	JsonNode *root_node = json_parser_get_root(parser);
	if (root_node == nullptr
			|| json_node_get_node_type(root_node) != JSON_NODE_OBJECT) {
		return nullptr;
	}
	JsonObject *root = json_node_get_object(root_node);
	return root != nullptr && json_object_has_member(root, "bar")
		&& json_node_get_node_type(json_object_get_member(root, "bar")) == JSON_NODE_OBJECT
		? json_node_get_object(json_object_get_member(root, "bar")) : nullptr;
}

} // namespace

int settings_section_height(int plain, int detailed) {
	return settings_header + plain * settings_row + detailed * settings_choice
		+ (plain + detailed) * settings_gap;
}

int settings_card_height(int column) {
	return 2 * settings_pad + settings_tabs_h + 3 * settings_gap + column
		+ settings_sep + settings_footer;
}

int settings_card_top(int height, double level) {
	double y = std::max(8.0, std::min(level - height / 2.0,
		double(screen_h - height - 8)));
	return int(std::round(y));
}

// Where PanelCard puts the card: level with the gear button, clamped
// inside the output, one margin beside the strip on whichever edge the
// toolbar sits.
int settings_card_left(bool left_edge) {
	return left_edge ? toolbar_margin + toolbar_button + beside_gap
		: strip_left - beside_gap - settings_panel;
}

std::vector<Band>::const_iterator settings_band_at(const std::vector<Band> &bands,
	double y) {
	auto best = bands.end();
	double best_distance = screen_h;
	for (auto it = bands.begin(); it != bands.end(); it++) {
		double distance = std::abs(it->centre() - y);
		if (distance < best_distance) {
			best = it;
			best_distance = distance;
		}
	}
	return best;
}

std::pair<double, double> settings_tab_point(int i, int card_y) {
	int column = i % settings_tab_cols;
	int row = i / settings_tab_cols;
	return {settings_card_x + settings_pad + settings_tab_w * (column + 0.5),
		card_y + settings_pad + row * settings_row + settings_row / 2.0};
}

int settings_chosen_tab(cairo_surface_t *s, int card_y) {
	int chosen = -1;
	for (int i = 0; i < settings_tabs; i++) {
		// The tint is read above the label, where no glyph is.
		auto [cx, cy] = settings_tab_point(i, card_y);
		int x = int(cx);
		int y = int(cy) - settings_row / 2 + 4;
		bool tint = pixel_is(s, x, y, selected_dark);
		bool plain = pixel_is(s, x, y, shell_bg_dark);
		if (tint == plain || (tint && chosen >= 0)) {
			return -1;
		}
		if (tint) {
			chosen = i;
		}
	}
	return chosen;
}

bool settings_switch_is(cairo_surface_t *s, double y, bool on) {
	double centre = settings_card_x + settings_pad + settings_panel
		- 2 * settings_pad - 8 - settings_switch_w / 2.0;
	return on ? pixel_is(s, int(centre - 10), int(y), accent_dark)
		: pixel_is(s, int(centre + 10), int(y), track_dark);
}

bool settings_shell_kept() {
	return bar_layer() != nullptr && bar_surfaces_made == bar_surfaces_remembered;
}

bool shell_reloaded() {
	return bar_layer() != nullptr && bar_surfaces_made != bar_surfaces_remembered;
}

int weather_fetch_count() {
	std::string log = read_file(shot_dir + "/curl-standin.log");
	int count = 0;
	for (size_t at = log.find("wttr.in"); at != std::string::npos;
			at = log.find("wttr.in", at + 1)) {
		count++;
	}
	return count;
}

int weather_fetches = 0; // weather_fetch_count() before a host write

void notify_send(const char *summary) {
	const char *const argv[] = {"notify-send", "-a", "Tests", "-u", "critical",
		"-t", "60000", summary, static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid > 0) {
		msg_children.push_back(child.pid);
	}
	if (child.out >= 0) {
		close(child.out);
	}
}

std::vector<std::string> shell_bar_hidden() {
	std::vector<std::string> out;
	JsonParser *parser = json_parser_new();
	JsonObject *bar = shell_json_bar(parser);
	if (bar != nullptr && json_object_has_member(bar, "hidden")
			&& json_node_get_node_type(json_object_get_member(bar, "hidden"))
				== JSON_NODE_ARRAY) {
		JsonArray *hidden = json_node_get_array(json_object_get_member(bar, "hidden"));
		for (guint i = 0; i < json_array_get_length(hidden); i++) {
			const char *name = json_node_get_string(json_array_get_element(hidden, i));
			if (name != nullptr) {
				out.push_back(name);
			}
		}
	}
	g_object_unref(parser);
	return out;
}

} // namespace bar
