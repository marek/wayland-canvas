#pragma once

#include <expected>
#include <optional>
#include <string>
#include "core/settings.h"

class Output;
class Server;
typedef struct _JsonObject JsonObject;

// The params of a request, as the methods read them. Each reader's error
// names the param, ready to answer with.

// A boolean param the caller has already found on `params`.
std::expected<bool, std::string> req_bool(JsonObject *params, const char *name);
// A string param: nullopt when absent; an error naming it when not one.
std::expected<std::optional<std::string>, std::string> opt_string(
	JsonObject *params, const char *name);
// A whole-number param: nullopt when absent; an error naming it when not one.
std::expected<std::optional<long long>, std::string> opt_int(
	JsonObject *params, const char *name);
// The `scope` param: global when absent; an error when it names neither scope.
std::expected<settings::Scope, std::string> opt_scope(JsonObject *params);
// The `output` param by name; nullptr when absent and none is under the
// cursor. An error names a bad or unknown param.
std::expected<Output *, std::string> opt_output(Server &server, JsonObject *params);
