#include <expected>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "core/chord.h"
#include "core/json.h"
#include "core/server.h"
#include "core/settings.h"
#include "core/settings_file.h"
#include "platform/ipc_methods.h"
#include "platform/ipc_params.h"
#include "platform/ipc_server.h"
#include "platform/ipc_state.h"

namespace {

// The request of a shortcut.capture waits on the user's chord.
constexpr std::string_view capture_wait = "shortcut.capture";

// The settings.set usage, naming every param the table gives.
std::string settings_set_usage() {
	std::string out = "params: exactly one of action with keys, commands";
	for (std::size_t i = 0; i < settings::fields.size(); i++) {
		out += i + 1 == settings::fields.size() ? " or " : ", ";
		out += settings::fields[i].param;
	}
	return out;
}

// The value settings.set's param of `field`'s name asks for, as a value of the
// field: the index of a choice's name, 0 or 1 for a toggle, an integer's
// number. An integer is compared as the int64 it arrived as: narrowing first
// would wrap a number past an int into a valid one.
std::expected<long long, std::string> field_value(const settings::Field &field,
		JsonObject *params) {
	std::string param{field.param};
	switch (field.kind) {
	case settings::Kind::toggle: {
		std::expected<bool, std::string> on = req_bool(params, param.c_str());
		if (!on.has_value()) {
			return std::unexpected(on.error());
		}
		return *on ? 1 : 0;
	}
	case settings::Kind::choice: {
		std::expected<std::optional<std::string>, std::string> name =
			opt_string(params, param.c_str());
		if (!name.has_value()) {
			return std::unexpected(name.error());
		}
		std::optional<int> index = name->has_value()
			? settings::parse_value(field, **name) : std::nullopt;
		if (!index.has_value()) {
			return std::unexpected(std::format("params: {} must be {}", param,
				settings::allowed(field)));
		}
		return *index;
	}
	case settings::Kind::integer: {
		std::expected<std::optional<long long>, std::string> number =
			opt_int(params, param.c_str());
		if (!number.has_value()) {
			return std::unexpected(number.error());
		}
		if (!settings::allows(field, **number)) {
			// 0 is the user's "never" where the file spells it `off`.
			return std::unexpected(std::format("params: {} must be {} to {} {}{}", param,
				field.off ? 0 : field.min, field.max, field.unit,
				field.off ? ", 0 for off" : ""));
		}
		return **number;
	}
	}
	return std::unexpected(std::string{"params: not a setting"});
}

// scope + action + keys: `keys` null (or absent) writes the action's
// defaults into its line, `[]` leaves it keyless.
std::expected<settings::Edit, std::string> edit_action(settings::File &file,
		JsonObject *params) {
	std::expected<settings::Scope, std::string> scope = opt_scope(params);
	if (!scope.has_value()) {
		return std::unexpected(scope.error());
	}
	std::expected<std::optional<std::string>, std::string> action =
		opt_string(params, "action");
	if (!action.has_value()) {
		return std::unexpected(action.error());
	}
	if (!action->has_value() || (*action)->empty()) {
		return std::unexpected(std::string{
			"params: action must be a non-empty string"});
	}
	std::optional<std::vector<settings::Chord>> keys;
	if (json_object_has_member(params, "keys")) {
		JsonNode *node = json_object_get_member(params, "keys");
		if (json_node_get_node_type(node) == JSON_NODE_NULL) {
			keys = std::nullopt; // the reset: the defaults
		} else if (JsonArray *array = node_array(node)) {
			std::vector<settings::Chord> chords;
			for (unsigned i = 0; i < json_array_get_length(array); i++) {
				const char *key = node_string(json_array_get_element(array, i));
				if (key == nullptr) {
					return std::unexpected(std::string{
						"params: keys must be chord strings"});
				}
				std::expected<settings::Chord, std::string> chord =
					settings::parse_chord(key, *scope);
				if (!chord.has_value()) {
					return std::unexpected(std::format("bad chord '{}': {}",
						key, chord.error()));
				}
				chords.push_back(*chord);
			}
			keys = std::optional{std::move(chords)};
		} else {
			return std::unexpected(std::string{
				"params: keys must be an array of chords or null"});
		}
	}
	return file.set_action(*scope, *(*action), keys);
}

// The [commands] entries, replaced wholesale.
std::expected<settings::Edit, std::string> edit_commands(settings::File &file,
		JsonObject *params) {
	constexpr std::string_view usage = "params: commands must be an array of {keys, command}";
	JsonArray *array = node_array(json_object_get_member(params, "commands"));
	if (array == nullptr) {
		return std::unexpected(std::string{usage});
	}
	std::vector<settings::Command> commands;
	for (unsigned i = 0; i < json_array_get_length(array); i++) {
		JsonObject *entry = node_object(json_array_get_element(array, i));
		if (entry == nullptr) {
			return std::unexpected(std::string{usage});
		}
		const char *keys_text = nullptr;
		const char *command_text = nullptr;
		for (const char *name : {"keys", "command"}) {
			if (!json_object_has_member(entry, name)) {
				continue;
			}
			const char *value = node_string(json_object_get_member(entry, name));
			if (value == nullptr) {
				return std::unexpected(std::format(
					"params: commands' {} must be a string", name));
			}
			if (name == std::string_view{"keys"}) {
				keys_text = value;
			} else {
				command_text = value;
			}
		}
		if (keys_text == nullptr || command_text == nullptr) {
			return std::unexpected(std::string{
				"params: every command needs keys and command"});
		}
		std::expected<settings::Chord, std::string> chord =
			settings::parse_chord(keys_text, settings::Scope::global);
		if (!chord.has_value()) {
			return std::unexpected(std::format("bad chord '{}': {}",
				keys_text, chord.error()));
		}
		commands.push_back({*chord, command_text});
	}
	return file.set_commands(commands);
}

// What `shortcut.capture` answers: the chord pressed, or null when it was cancelled.
std::string chord_answer(std::optional<settings::Chord> chord) {
	return object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "keys");
		if (chord.has_value()) {
			json_builder_add_string_value(builder, settings::chord_text(*chord).c_str());
		} else {
			json_builder_add_null_value(builder);
		}
	});
}

IpcServer::Reply settings_get(Server &server) {
	return object_text([&](JsonBuilder *builder) {
		add_settings(builder, server);
	});
}

IpcServer::Reply settings_set(Server &server, JsonObject *params) {
	if (params == nullptr) {
		return std::unexpected(settings_set_usage());
	}
	bool has_action = json_object_has_member(params, "action");
	bool has_commands = json_object_has_member(params, "commands");
	const settings::Field *field = nullptr;
	int named = has_action + has_commands;
	for (const settings::Field &each : settings::fields) {
		if (json_object_has_member(params, std::string{each.param}.c_str())) {
			field = &each;
			named++;
		}
	}
	if (named != 1) {
		return std::unexpected(settings_set_usage());
	}
	settings::File file{server.conf.text()};
	std::expected<settings::Edit, std::string> edit = std::unexpected(std::string{});
	if (field != nullptr) {
		std::expected<long long, std::string> value = field_value(*field, params);
		if (!value.has_value()) {
			return std::unexpected(value.error());
		}
		edit = file.set(*field, *value);
	} else if (has_action) {
		edit = edit_action(file, params);
	} else {
		edit = edit_commands(file, params);
	}
	if (!edit.has_value()) {
		return std::unexpected(edit.error());
	}
	if (!server.conf.write(edit->text)) {
		return std::unexpected(std::format("{} is unwritable",
			server.conf.path()));
	}
	// The settings now, with where a taken chord came from.
	return object_text([&](JsonBuilder *builder) {
		add_settings(builder, server);
		json_builder_set_member_name(builder, "moved");
		json_builder_begin_array(builder);
		for (const settings::Moved &moved : edit->moved) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "scope");
			json_builder_add_string_value(builder,
				std::string{settings::scope_name(moved.scope)}.c_str());
			json_builder_set_member_name(builder, "action");
			json_builder_add_string_value(builder, moved.action.c_str());
			if (!moved.command.empty()) {
				json_builder_set_member_name(builder, "command");
				json_builder_add_string_value(builder, moved.command.c_str());
			}
			json_builder_set_member_name(builder, "keys");
			json_builder_begin_array(builder);
			for (settings::Chord chord : moved.keys) {
				json_builder_add_string_value(builder,
					settings::chord_text(chord).c_str());
			}
			json_builder_end_array(builder);
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
	});
}

IpcServer::Reply shortcut_capture(IpcServer &ipc, Server &server, JsonObject *params) {
	// The scope is only named for what settings.set will decide; the
	// capture reports the chord as pressed.
	std::expected<settings::Scope, std::string> scope = opt_scope(params);
	if (!scope.has_value()) {
		return std::unexpected(scope.error());
	}
	// A capture under way is answered null as this one begins.
	server.input.shortcut_capture.begin([&ipc](std::optional<settings::Chord> chord) {
		ipc.answer(capture_wait, chord_answer(chord));
	});
	return IpcServer::Wait{capture_wait};
}

IpcServer::Reply shortcut_cancel(Server &server) {
	// A capture under way is cancelled: its client is answered null now.
	server.input.shortcut_capture.cancel();
	return std::string{"true"};
}

} // namespace

void register_settings_methods(IpcServer &ipc, Server &server) {
	// The capture ends with its client: no keys are swallowed for a dead one.
	ipc.on_abandoned(capture_wait, [&server] { server.input.shortcut_capture.cancel(); });
	ipc.register_method("settings.get", [&server](JsonObject *) { return settings_get(server); });
	ipc.register_method("settings.set",
		[&server](JsonObject *params) { return settings_set(server, params); });
	ipc.register_method("shortcut.capture", [&ipc, &server](JsonObject *params) {
		return shortcut_capture(ipc, server, params);
	});
	ipc.register_method("shortcut.cancel", [&server](JsonObject *) { return shortcut_cancel(server); });
}
