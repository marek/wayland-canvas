#include <format>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/server.h"
#include "platform/ipc_methods.h"
#include "platform/ipc_params.h"
#include "platform/ipc_server.h"
#include "platform/ipc_state.h"
#include "platform/output.h"

namespace {

IpcServer::Reply state(Server &server) {
	return object_text([&](JsonBuilder *builder) {
		add_outputs_array(builder, server);
		// Where the cursor is on the canvas, on the output it is on: a
		// read for the moment of the answer — no event carries it, so a
		// client that needs it asks.
		json_builder_set_member_name(builder, "cursor");
		if (Output *under = server.output_under_cursor()) {
			Point at = under->local_to_canvas(server.cursor->x - under->lx,
				server.cursor->y - under->ly);
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "x");
			json_builder_add_double_value(builder, at.x);
			json_builder_set_member_name(builder, "y");
			json_builder_add_double_value(builder, at.y);
			json_builder_set_member_name(builder, "output");
			json_builder_add_string_value(builder, under->wlr->name);
			json_builder_end_object(builder);
		} else {
			json_builder_add_null_value(builder);
		}
		add_desktops_array(builder, server);
		std::vector<WindowRow> windows;
		collect_windows(server, windows);
		add_windows_array(builder, windows);
		json_builder_set_member_name(builder, "theme");
		std::string mode = std::string{theme_name(server.theme.mode())};
		json_builder_add_string_value(builder, mode.c_str());
		json_builder_set_member_name(builder, "recording");
		if (server.capture_ui.recording()) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "path");
			std::string path = server.capture_ui.recording_path();
			json_builder_add_string_value(builder, path.c_str());
			json_builder_end_object(builder);
		} else {
			json_builder_add_null_value(builder);
		}
	});
}

IpcServer::Reply theme_set(Server &server, JsonObject *params) {
	constexpr std::string_view usage =
		"params: mode must be \"light\" or \"dark\"";
	std::expected<std::optional<std::string>, std::string> mode =
		opt_string(params, "mode");
	if (!mode.has_value() || !mode->has_value()) {
		return std::unexpected(std::string{usage});
	}
	if (*(*mode) != "light" && *(*mode) != "dark") {
		return std::unexpected(std::format(
			"params: mode must be \"light\" or \"dark\", not '{}'", *(*mode)));
	}
	server.set_theme(*(*mode) == "light" ? ThemeMode::light : ThemeMode::dark);
	return std::string{"true"};
}

} // namespace

void register_state_methods(IpcServer &ipc, Server &server) {
	ipc.register_method("state", [&server](JsonObject *) { return state(server); });
	ipc.register_method("theme.set",
		[&server](JsonObject *params) { return theme_set(server, params); });
}
