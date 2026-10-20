#pragma once

#include <json-glib/json-glib.h>

#include <string_view>

#include "core/handle.h"

using JsonParserPtr = Handle<JsonParser, g_object_unref>;
using JsonBuilderPtr = Handle<JsonBuilder, g_object_unref>;
using JsonNodePtr = Handle<JsonNode, json_node_unref>;
using JsonGeneratorPtr = Handle<JsonGenerator, g_object_unref>;
// Text json-glib returns, such as json_to_string's.
using GCharPtr = Handle<char, g_free>;
using GErrorPtr = Handle<GError, g_error_free>;

// Parses one JSON document; null when it is not valid JSON, and then json-glib's reason
// goes to `reason` when one is given (empty text has none).
inline JsonParserPtr parse_json(std::string_view text, GErrorPtr *reason = nullptr) {
	if (text.empty()) {
		return nullptr;
	}
	JsonParserPtr parser(json_parser_new());
	GError *error = nullptr;
	if (!json_parser_load_from_data(parser.get(), text.data(),
			static_cast<gssize>(text.size()), reason != nullptr ? &error : nullptr)) {
		if (reason != nullptr) {
			reason->reset(error);
		}
		return nullptr;
	}
	return parser;
}

// The document's root when it is an object; null for no document or any other root.
// The object lives as long as `parser`.
inline JsonObject *root_object(const JsonParserPtr &parser) {
	if (parser == nullptr) {
		return nullptr;
	}
	JsonNode *root = json_parser_get_root(parser.get());
	return root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
}
