#include "platform/ipc_params.h"

#include <format>
#include <memory>

#include "core/json.h"
#include "core/server.h"
#include "platform/output.h"

std::expected<bool, std::string> req_bool(JsonObject *params, const char *name) {
	JsonNode *node = json_object_get_member(params, name);
	if (json_node_get_node_type(node) != JSON_NODE_VALUE
			|| json_node_get_value_type(node) != G_TYPE_BOOLEAN) {
		return std::unexpected(std::format(
			"params: {} must be true or false", name));
	}
	return static_cast<bool>(json_node_get_boolean(node));
}

std::expected<std::optional<std::string>, std::string> opt_string(
		JsonObject *params, const char *name) {
	if (params == nullptr || !json_object_has_member(params, name)) {
		return std::optional<std::string>{};
	}
	const char *value = node_string(json_object_get_member(params, name));
	if (value == nullptr) {
		return std::unexpected(std::format("params: {} must be a string", name));
	}
	return std::optional{std::string{value}};
}

std::expected<std::optional<long long>, std::string> opt_int(
		JsonObject *params, const char *name) {
	if (params == nullptr || !json_object_has_member(params, name)) {
		return std::optional<long long>{};
	}
	JsonNode *node = json_object_get_member(params, name);
	if (json_node_get_node_type(node) != JSON_NODE_VALUE
			|| json_node_get_value_type(node) != G_TYPE_INT64) {
		return std::unexpected(std::format("params: {} must be a whole number", name));
	}
	return std::optional{json_node_get_int(node)};
}

std::expected<settings::Scope, std::string> opt_scope(JsonObject *params) {
	std::expected<std::optional<std::string>, std::string> name =
		opt_string(params, "scope");
	if (!name.has_value()) {
		return std::unexpected(name.error());
	}
	if (!name->has_value()) {
		return settings::Scope::global;
	}
	std::optional<settings::Scope> scope = settings::parse_scope(*(*name));
	if (!scope.has_value()) {
		return std::unexpected(std::format("params: scope must be \"{}\" or \"{}\"",
			settings::scope_name(settings::Scope::global),
			settings::scope_name(settings::Scope::overview)));
	}
	return *scope;
}

std::expected<Output *, std::string> opt_output(Server &server, JsonObject *params) {
	std::expected<std::optional<std::string>, std::string> name =
		opt_string(params, "output");
	if (!name.has_value()) {
		return std::unexpected(name.error());
	}
	if (!name->has_value()) {
		return server.output_under_cursor();
	}
	for (const std::unique_ptr<Output> &output : server.outputs) {
		if (*(*name) == output->wlr->name) {
			return output.get();
		}
	}
	return std::unexpected(std::format("unknown output '{}'", *(*name)));
}
