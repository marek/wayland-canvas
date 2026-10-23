#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include "core/server.h"
#include "platform/ipc_methods.h"
#include "platform/ipc_params.h"
#include "platform/ipc_server.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

IpcServer::Reply launcher_toggle(Server &server, JsonObject *params) {
	std::expected<Output *, std::string> output = opt_output(server, params);
	if (!output.has_value()) {
		return std::unexpected(output.error());
	}
	server.launcher.toggle(*output); // nullptr: no output, nothing to toggle
	return std::string{"true"};
}

IpcServer::Reply overview_toggle(Server &server, JsonObject *params) {
	std::expected<Output *, std::string> output = opt_output(server, params);
	if (!output.has_value()) {
		return std::unexpected(output.error());
	}
	if (*output == nullptr) {
		return std::unexpected(std::string{"no output under the cursor"});
	}
	Output &o = **output;
	if (o.overview.active()) {
		o.overview.exit();
	} else {
		o.overview.enter();
	}
	return std::string{"true"};
}

// Goes to a window by its session id: the same restore path a shelf click
// takes (docs/intents/windows/window-management.md).
IpcServer::Reply window_focus(Server &server, JsonObject *params) {
	std::expected<std::optional<long long>, std::string> id =
		opt_int(params, "window");
	if (!id.has_value()) {
		return std::unexpected(id.error());
	}
	if (!id->has_value()) {
		return std::unexpected(std::string{"params: window must be a number"});
	}
	auto wanted = static_cast<uint64_t>(**id);
	for (const std::unique_ptr<View> &view : server.toplevels) {
		if (view->session_id == wanted && view->mapped) {
			view->activate();
			return std::string{"true"};
		}
	}
	return std::unexpected(std::format("unknown window id '{}'", wanted));
}

} // namespace

void register_window_methods(IpcServer &ipc, Server &server) {
	ipc.register_method("launcher.toggle",
		[&server](JsonObject *params) { return launcher_toggle(server, params); });
	ipc.register_method("overview.toggle",
		[&server](JsonObject *params) { return overview_toggle(server, params); });
	ipc.register_method("window.focus",
		[&server](JsonObject *params) { return window_focus(server, params); });
}
