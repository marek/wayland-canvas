#pragma once

#include <json-glib/json-glib.h>

#include <string_view>

#include "core/handle.h"

using JsonParserPtr = Handle<JsonParser, g_object_unref>;
using JsonBuilderPtr = Handle<JsonBuilder, g_object_unref>;
using JsonNodePtr = Handle<JsonNode, json_node_unref>;
// Text json-glib returns, such as json_to_string's.
using GCharPtr = Handle<char, g_free>;

// Parses one JSON document; null when it is not valid JSON.
inline JsonParserPtr parse_json(std::string_view text) {
	if (text.empty()) {
		return nullptr;
	}
	JsonParserPtr parser(json_parser_new());
	if (!json_parser_load_from_data(parser.get(), text.data(),
			static_cast<gssize>(text.size()), nullptr)) {
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
