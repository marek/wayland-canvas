#include "platform/ipc_state.h"

#include <algorithm>
#include <memory>
#include <string>

#include "canvas/desktop.h"
#include "core/chord.h"
#include "core/json.h"
#include "core/server.h"
#include "core/settings.h"
#include "platform/layer.h"
#include "platform/output.h"

namespace {

// The output-local rectangle the chrome leaves of the screen: the bars'
// exclusive zones (Output::usable), less the overview's strips while the
// overview shows. A strip's window is its settled square plus the pocket
// that overhangs the edge it hugs — the strip lies against the window's
// inner side — so the free box ends at the window's far edge.
wlr_box free_screen_box(const Output &output) {
	wlr_box free = output.usable;
	if (!output.overview.active()) {
		return free;
	}
	for (const auto &surfaces : output.layers) {
		for (const std::unique_ptr<LayerSurface> &layer : surfaces) {
			if (!layer->mapped || !layer->is_overview_strip()) {
				continue;
			}
			const wlr_box &geo = layer->geo;
			if (geo.x < 0) { // hugs the left edge, pocket past it
				int inner = std::max(free.x, geo.x + geo.width);
				free.width = std::max(0, free.x + free.width - inner);
				free.x = inner;
			} else if (geo.y < 0) { // the top edge
				int inner = std::max(free.y, geo.y + geo.height);
				free.height = std::max(0, free.y + free.height - inner);
				free.y = inner;
			} else if (geo.x + geo.width > output.width) { // the right edge
				free.width = std::max(0,
					std::min(free.x + free.width, geo.x) - free.x);
			} else if (geo.y + geo.height > output.height) { // the bottom edge
				free.height = std::max(0,
					std::min(free.y + free.height, geo.y) - free.y);
			}
		}
	}
	return free;
}

void add_output(JsonBuilder *builder, const Output &output) {
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, output.wlr->name);
	add_rect_members(builder, output.lx, output.ly, output.width, output.height);
	json_builder_set_member_name(builder, "scale");
	json_builder_add_double_value(builder, output.wlr->scale);
	// What the output is looking at, in canvas pixels: a plugin placing
	// something on the plane puts it where the user is looking.
	Rect view = output.viewport();
	json_builder_set_member_name(builder, "view");
	json_builder_begin_object(builder);
	add_rect_members(builder, view.x, view.y, view.w, view.h);
	json_builder_end_object(builder);
	// What that view leaves free of the chrome, in canvas pixels — the
	// bars' exclusive zones, and the overview's strips while the overview
	// shows: where a plugin keeps something it puts where the user is
	// looking clear of them.
	wlr_box screen = free_screen_box(output);
	Point tl = output.local_to_canvas(screen.x, screen.y);
	Point br = output.local_to_canvas(screen.x + screen.width,
		screen.y + screen.height);
	json_builder_set_member_name(builder, "usable");
	json_builder_begin_object(builder);
	add_rect_members(builder, tl.x, tl.y, br.x - tl.x, br.y - tl.y);
	json_builder_end_object(builder);
	json_builder_set_member_name(builder, "overview");
	json_builder_add_boolean_value(builder, output.overview.active());
	json_builder_end_object(builder);
}

// A setting's value as JSON: a choice's name, a toggle's boolean, an
// integer's number.
void add_field_value(JsonBuilder *builder, const settings::Field &field, int value) {
	switch (field.kind) {
	case settings::Kind::choice:
		json_builder_add_string_value(builder,
			std::string{field.values()[static_cast<std::size_t>(value)]}.c_str());
		break;
	case settings::Kind::toggle:
		json_builder_add_boolean_value(builder, value != 0);
		break;
	case settings::Kind::integer:
		json_builder_add_int_value(builder, value);
		break;
	}
}

// What one setting takes, as the `fields` of settings.get report it:
// where it is written, its kind, its bounds or names, and its default.
void add_field_limits(JsonBuilder *builder, const settings::Field &field) {
	auto add_string = [builder](const char *name, std::string_view text) {
		json_builder_set_member_name(builder, name);
		json_builder_add_string_value(builder, std::string(text).c_str());
	};
	add_string("section", field.section);
	add_string("key", field.key);
	switch (field.kind) {
	case settings::Kind::choice:
		add_string("kind", "choice");
		json_builder_set_member_name(builder, "choices");
		json_builder_begin_array(builder);
		for (std::string_view name : field.values()) {
			json_builder_add_string_value(builder, std::string{name}.c_str());
		}
		json_builder_end_array(builder);
		break;
	case settings::Kind::toggle:
		add_string("kind", "toggle");
		break;
	case settings::Kind::integer:
		add_string("kind", "integer");
		add_string("unit", field.unit);
		json_builder_set_member_name(builder, "min");
		json_builder_add_int_value(builder, field.off ? 0 : field.min);
		json_builder_set_member_name(builder, "max");
		json_builder_add_int_value(builder, field.max);
		break;
	}
	json_builder_set_member_name(builder, "default");
	add_field_value(builder, field, field.fallback);
}

} // namespace

std::string_view theme_name(ThemeMode mode) {
	return mode == ThemeMode::light ? "light" : "dark";
}

void add_outputs_array(JsonBuilder *builder, Server &server) {
	json_builder_set_member_name(builder, "outputs");
	json_builder_begin_array(builder);
	for (const std::unique_ptr<Output> &output : server.outputs) {
		add_output(builder, *output);
	}
	json_builder_end_array(builder);
}

void add_desktops_array(JsonBuilder *builder, Server &server) {
	json_builder_set_member_name(builder, "desktops");
	json_builder_begin_array(builder);
	for (const Desktop &desktop : server.canvas.desktops.all()) {
		const wlr_box &frame = desktop.frame();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, static_cast<int>(desktop.id()));
		json_builder_set_member_name(builder, "number");
		json_builder_add_int_value(builder, desktop.number());
		add_rect_members(builder, frame.x, frame.y, frame.width, frame.height);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
}

void collect_windows(Server &server, std::vector<WindowRow> &rows) {
	std::size_t count = 0;
	// toplevels is creation order; views is the stacking order.
	for (const std::unique_ptr<View> &owned : server.toplevels) {
		View *view = owned.get();
		if (!view->mapped) {
			continue; // nothing to go to before it maps
		}
		if (count == rows.size()) {
			rows.emplace_back();
		}
		WindowRow &row = rows[count++];
		const Desktop *desktop = server.canvas.desktop_of(*view);
		row.id = view->session_id;
		row.app_id.assign(view->shell_app_id() != nullptr ? view->shell_app_id() : "");
		row.title.assign(view->shell_title() != nullptr ? view->shell_title() : "");
		row.desktop = id_of(desktop);
		row.minimized = view->minimized;
		row.focused = server.focus.focused_view() == view;
	}
	rows.resize(count);
}

void add_windows_array(JsonBuilder *builder, const std::vector<WindowRow> &rows) {
	json_builder_set_member_name(builder, "windows");
	json_builder_begin_array(builder);
	for (const WindowRow &row : rows) {
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, static_cast<gint64>(row.id));
		json_builder_set_member_name(builder, "appId");
		json_builder_add_string_value(builder, row.app_id.c_str());
		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder, row.title.c_str());
		json_builder_set_member_name(builder, "desktop");
		if (row.desktop.has_value()) {
			json_builder_add_int_value(builder, static_cast<int>(*row.desktop));
		} else {
			json_builder_add_null_value(builder);
		}
		json_builder_set_member_name(builder, "minimized");
		json_builder_add_boolean_value(builder, row.minimized);
		json_builder_set_member_name(builder, "focused");
		json_builder_add_boolean_value(builder, row.focused);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
}

void add_settings(JsonBuilder *builder, Server &server) {
	auto add_string = [builder](const char *name, std::string_view text) {
		json_builder_set_member_name(builder, name);
		// json-glib copies the string, so a temporary ending now is fine.
		json_builder_add_string_value(builder, std::string(text).c_str());
	};
	auto add_chords = [builder](const std::vector<settings::Chord> &chords) {
		json_builder_begin_array(builder);
		for (settings::Chord chord : chords) {
			json_builder_add_string_value(builder,
				settings::chord_text(chord).c_str());
		}
		json_builder_end_array(builder);
	};
	add_string("path", server.conf.path());
	json_builder_set_member_name(builder, "errors");
	json_builder_begin_array(builder);
	for (const settings::Error &error : server.conf.errors()) {
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "line");
		json_builder_add_int_value(builder, error.line);
		add_string("text", error.text);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "shortcuts");
	json_builder_begin_array(builder);
	const std::vector<settings::ActionDef> &registry = settings::registry();
	for (std::size_t i = 0; i < registry.size(); i++) {
		const settings::ActionDef &def = registry[i];
		json_builder_begin_object(builder);
		add_string("scope", settings::scope_name(def.scope));
		add_string("action", def.name);
		add_string("label", def.label);
		add_string("group", def.group);
		json_builder_set_member_name(builder, "keys");
		add_chords(server.conf.resolved.keys[i]);
		json_builder_set_member_name(builder, "defaults");
		add_chords(def.defaults);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "commands");
	json_builder_begin_array(builder);
	for (const settings::Command &command : server.conf.resolved.commands) {
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "keys");
		json_builder_add_string_value(builder,
			settings::chord_text(command.chord).c_str());
		add_string("command", command.command);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	// [gestures] and [session]: each setting's value, a section at a time.
	std::string_view section;
	for (const settings::Field &field : settings::fields) {
		if (field.section != section) {
			if (!section.empty()) {
				json_builder_end_object(builder);
			}
			section = field.section;
			json_builder_set_member_name(builder, std::string{section}.c_str());
			json_builder_begin_object(builder);
		}
		json_builder_set_member_name(builder, std::string{field.param}.c_str());
		add_field_value(builder, field, field.read(server.conf.resolved));
	}
	json_builder_end_object(builder);
	json_builder_set_member_name(builder, "fields");
	json_builder_begin_object(builder);
	for (const settings::Field &field : settings::fields) {
		json_builder_set_member_name(builder, std::string{field.param}.c_str());
		json_builder_begin_object(builder);
		add_field_limits(builder, field);
		json_builder_end_object(builder);
	}
	json_builder_end_object(builder);
}
