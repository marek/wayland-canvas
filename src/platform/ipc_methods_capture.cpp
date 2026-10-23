#include <array>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "apps/capture_picker.h"
#include "core/json.h"
#include "core/server.h"
#include "platform/ipc_methods.h"
#include "platform/ipc_params.h"
#include "platform/ipc_server.h"
#include "platform/output.h"

namespace {

// The request of a capture.pick waits on the user's pick.
constexpr std::string_view pick_wait = "capture.pick";

std::optional<PickWhat> pick_what(std::string_view name) {
	constexpr std::array kinds = {
		std::pair{std::string_view{"region"}, PickWhat::region},
		std::pair{std::string_view{"window"}, PickWhat::window},
		std::pair{std::string_view{"output"}, PickWhat::output},
		std::pair{std::string_view{"any"}, PickWhat::any},
	};
	for (const auto &[known, kind] : kinds) {
		if (known == name) {
			return kind;
		}
	}
	return std::nullopt;
}

// What `capture.pick` answers for the user's pick, or for none.
std::expected<std::string, std::string> pick_answer(
		const std::optional<CapturePicker::Pick> &pick) {
	if (!pick.has_value()) {
		return std::unexpected(std::string{"cancelled"});
	}
	return object_text([&](JsonBuilder *builder) {
		switch (pick->kind) {
		case CapturePicker::Pick::Kind::region:
			json_builder_set_member_name(builder, "output");
			json_builder_add_string_value(builder, pick->output->wlr->name);
			json_builder_set_member_name(builder, "x");
			json_builder_add_int_value(builder, pick->area.x);
			json_builder_set_member_name(builder, "y");
			json_builder_add_int_value(builder, pick->area.y);
			json_builder_set_member_name(builder, "w");
			json_builder_add_int_value(builder, pick->area.width);
			json_builder_set_member_name(builder, "h");
			json_builder_add_int_value(builder, pick->area.height);
			break;
		case CapturePicker::Pick::Kind::window: {
			const char *identifier = pick->view->ext_toplevel_identifier();
			json_builder_set_member_name(builder, "window");
			json_builder_add_int_value(builder, static_cast<gint64>(pick->view->session_id));
			json_builder_set_member_name(builder, "identifier");
			json_builder_add_string_value(builder, identifier != nullptr ? identifier : "");
			break;
		}
		case CapturePicker::Pick::Kind::output:
			json_builder_set_member_name(builder, "output");
			json_builder_add_string_value(builder, pick->output->wlr->name);
			break;
		}
	});
}

IpcServer::Reply capture_pick(IpcServer &ipc, Server &server, JsonObject *params) {
	constexpr std::string_view usage =
		R"(params: what must be "region", "window", "output" or "any")";
	std::expected<std::optional<std::string>, std::string> what =
		opt_string(params, "what");
	std::optional<PickWhat> kind = what.has_value() && what->has_value()
		? pick_what(*(*what)) : std::nullopt;
	if (!kind.has_value()) {
		return std::unexpected(std::string{usage});
	}
	std::expected<Output *, std::string> output = opt_output(server, params);
	if (!output.has_value()) {
		return std::unexpected(output.error());
	}
	if (*output == nullptr) {
		return std::unexpected(std::string{"no output to pick on"});
	}
	bool started = !ipc.waiting(pick_wait)
		&& server.capture_ui.ask_pick(*output, *kind,
			[&ipc](std::optional<CapturePicker::Pick> pick) {
				ipc.answer(pick_wait, pick_answer(pick));
			});
	if (!started) {
		return std::unexpected(std::string{"a capture pick is already running"});
	}
	return IpcServer::Wait{pick_wait};
}

IpcServer::Reply capture_stop(Server &server) {
	if (!server.capture_ui.recording()) {
		return std::unexpected(std::string{"not recording"});
	}
	server.capture_ui.stop_recording();
	return std::string{"true"};
}

} // namespace

void register_capture_methods(IpcServer &ipc, Server &server) {
	ipc.register_method("capture.pick", [&ipc, &server](JsonObject *params) {
		return capture_pick(ipc, server, params);
	});
	ipc.register_method("capture.stop", [&server](JsonObject *) { return capture_stop(server); });
}
