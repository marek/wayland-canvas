// The toolbar's edges and the app bar: where the strips sit and what a flyout shows.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr const char *app_bar_namespace = "wayland-canvas-app-bar";

// Writes one key of one section into the scratch shell.json, keeping
// everything else it holds: an edit the host sees, so the shell reloads
// with it. `edge` names a string value; `on` a boolean one.
bool set_shell_config(const char *section, const char *key,
		const char *edge, const bool *on = nullptr) {
	JsonParser *parser = json_parser_new();
	std::string text = read_file(shell_config);
	if (text.empty()) {
		text = "{}"; // no shell.json yet is the shell's defaults
	}
	bool ok = json_parser_load_from_data(parser, text.c_str(),
			static_cast<gssize>(text.size()), nullptr)
		&& json_node_get_node_type(json_parser_get_root(parser))
			== JSON_NODE_OBJECT;
	if (ok) {
		JsonObject *config = json_node_get_object(json_parser_get_root(parser));
		JsonNode *held = json_object_get_member(config, section);
		if (held == nullptr
				|| json_node_get_node_type(held) != JSON_NODE_OBJECT) {
			json_object_set_object_member(config, section, json_object_new());
		}
		JsonObject *object = json_node_get_object(
			json_object_get_member(config, section));
		if (on != nullptr) {
			json_object_set_boolean_member(object, key, *on);
		} else {
			json_object_set_string_member(object, key, edge);
		}
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

// The app bar's length along the edge it shares with the toolbar, 0 when it
// is on another edge or absent: the two are centred as one strip, so the
// toolbar's own centring is only its share of it.
int shared_apps_length(const wlr_box &toolbar) {
	LayerSurface *apps = app_bar_layer();
	if (apps == nullptr) {
		return 0;
	}
	wlr_box strip = app_bar_strip_box(apps->geo);
	bool column = toolbar.width == toolbar_button;
	// A row of one app is a square as a strip: only its surface, a column's
	// being the strip plus the pocket across, tells which it is.
	bool apps_column = apps->geo.width == toolbar_button + toolbar_margin + toolbar_button;
	if (column != apps_column) {
		return 0; // one is a column, the other a row: different edges
	}
	if (column ? strip.x != toolbar.x : strip.y != toolbar.y) {
		return 0; // the same orientation on opposite edges
	}
	return column ? strip.height : strip.width;
}

} // namespace

bool set_toolbar_position(const char *edge) {
	return set_shell_config("toolbar", "position", edge);
}

std::string toolbar_position_in_config() {
	JsonParser *parser = json_parser_new();
	std::string named;
	if (json_parser_load_from_file(parser, shell_config.c_str(), nullptr)
		&& json_node_get_node_type(json_parser_get_root(parser))
			== JSON_NODE_OBJECT) {
		JsonObject *config = json_node_get_object(json_parser_get_root(parser));
		JsonNode *toolbar = json_object_get_member(config, "toolbar");
		JsonObject *toolbar_object = toolbar != nullptr
			&& json_node_get_node_type(toolbar) == JSON_NODE_OBJECT
			? json_node_get_object(toolbar) : nullptr;
		if (toolbar_object != nullptr
				&& json_object_has_member(toolbar_object, "position")) {
			JsonNode *position = json_object_get_member(toolbar_object, "position");
			if (position != nullptr
					&& json_node_get_value_type(position) == G_TYPE_STRING) {
				named = json_node_get_string(position);
			}
		}
	}
	g_object_unref(parser);
	return named;
}

bool set_app_bar_position(const char *edge) {
	return set_shell_config("appbar", "position", edge);
}

bool set_app_bar_enabled(bool on) {
	return set_shell_config("appbar", "enabled", nullptr, &on);
}

wlr_box toolbar_strip_box(const wlr_box &geo) {
	if (geo.width == toolbar_button + toolbar_margin + toolbar_button) { // a column
		return {geo.x > screen_w / 2 ? geo.x : geo.x + toolbar_margin + toolbar_button,
			geo.y, toolbar_button, geo.height};
	}
	// a row: the top edge's pocket reaches past the screen's top
	int pocket = geo.y < 0 ? bar_height() + toolbar_margin + toolbar_button
		: toolbar_margin + toolbar_button;
	return {geo.x, geo.y < 0 ? geo.y + pocket : geo.y, geo.width, toolbar_button};
}

bool strip_on_edge(LayerSurface *layer, const char *edge) {
	if (layer == nullptr) {
		return false;
	}
	wlr_box geo = toolbar_strip_box(layer->geo);
	bool column = std::strcmp(edge, "left") == 0 || std::strcmp(edge, "right") == 0;
	if (column && (geo.width != toolbar_button || geo.height % toolbar_button != 0)) {
		return false;
	}
	if (!column && (geo.height != toolbar_button || geo.width % toolbar_button != 0)) {
		return false;
	}
	if (std::strcmp(edge, "right") == 0) {
		return geo.x == strip_left;
	}
	if (std::strcmp(edge, "left") == 0) {
		return geo.x == toolbar_margin;
	}
	if (std::strcmp(edge, "top") == 0) {
		return geo.y == bar_height() + toolbar_margin;
	}
	return geo.y + geo.height == screen_h - toolbar_margin;
}

bool toolbar_on_edge(const char *edge) {
	return strip_on_edge(toolbar_layer(), edge);
}

LayerSurface *app_bar_layer() {
	return named_layer(app_bar_namespace);
}

wlr_box app_bar_strip_box(const wlr_box &geo) {
	return toolbar_strip_box(geo);
}

LayerSurface *app_bar_flyout_layer() {
	return named_layer("wayland-canvas-app-bar-chrome");
}

int app_bar_windows_of(const View *view) {
	if (view == nullptr) {
		return 0;
	}
	const char *app_id = view->shell_app_id();
	int n = 0;
	for (const std::unique_ptr<View> &other : server->toplevels) {
		if (!other->mapped) {
			continue;
		}
		const char *id = other->shell_app_id();
		bool same = (app_id == nullptr || *app_id == '\0')
			? other.get() == view
			: (id != nullptr && std::strcmp(id, app_id) == 0);
		if (same) {
			n++;
		}
	}
	return n;
}

std::pair<double, double> app_bar_flyout_row(const wlr_box &entry,
		const View *view) {
	constexpr int card_w = 240;
	constexpr int pad = 10;
	constexpr int gap = 2;
	int top = 0, height = 0, total = 2 * pad;
	bool before = true;
	const char *app_id = view != nullptr ? view->shell_app_id() : nullptr;
	for (const std::unique_ptr<View> &other : server->toplevels) {
		if (!other->mapped) {
			continue;
		}
		const char *id = other->shell_app_id();
		bool same = (app_id == nullptr || *app_id == '\0')
			? other.get() == view
			: (id != nullptr && std::strcmp(id, app_id) == 0);
		if (!same) {
			continue;
		}
		int row = other->minimized ? settings_row + settings_detail : settings_row;
		if (other.get() == view) {
			before = false;
			height = row;
		} else if (before) {
			top += row + gap;
		}
		total += row + gap;
	}
	total -= gap;
	double card_x = entry.x > screen_w / 2
		? entry.x - toolbar_margin - card_w : entry.x + entry.width + toolbar_margin;
	double card_y = std::max(double(toolbar_margin),
		std::min(double(entry.y), double(screen_h - total - toolbar_margin)));
	return {card_x + card_w / 2.0, card_y + pad + top + height / 2.0};
}

bool toolbar_centred(const wlr_box &geo) {
	bool column = geo.width == toolbar_button;
	int apps = shared_apps_length(geo);
	int length = column ? geo.height : geo.width;
	if (apps > 0) {
		length += strip_separator + apps;
	}
	return column ? geo.y + length / 2 == screen_h / 2
		: geo.x + length / 2 == screen_w / 2;
}

bool toolbar_settled_on(const char *edge) {
	LayerSurface *toolbar = toolbar_layer();
	return toolbar != nullptr && toolbar_on_edge(edge)
		&& toolbar_centred(toolbar_strip_box(toolbar->geo));
}

} // namespace bar
