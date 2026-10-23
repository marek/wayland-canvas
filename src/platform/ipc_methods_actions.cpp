#include <expected>
#include <optional>
#include <string>

#include "core/json.h"
#include "core/server.h"
#include "platform/ipc_methods.h"
#include "platform/ipc_params.h"
#include "platform/ipc_server.h"

namespace {

// `action.run` only delivers the event: the action itself belongs to a plugin.
IpcServer::Reply action_run(Server &server, JsonObject *params) {
	constexpr std::string_view usage =
		"params: name must be a non-empty string, args an object";
	std::expected<std::optional<std::string>, std::string> name =
		opt_string(params, "name");
	if (!name.has_value() || !name->has_value() || (*name)->empty()) {
		return std::unexpected(std::string{usage});
	}
	std::string args_json = "{}";
	if (params != nullptr && json_object_has_member(params, "args")) {
		JsonNode *args = json_object_get_member(params, "args");
		if (node_object(args) == nullptr) {
			return std::unexpected(std::string{usage});
		}
		args_json = node_text(args);
	}
	server.ipc_events.emit_action(*(*name), args_json);
	return std::string{"true"};
}

} // namespace

void register_action_methods(IpcServer &ipc, Server &server) {
	ipc.register_method("action.run",
		[&server](JsonObject *params) { return action_run(server, params); });
}
