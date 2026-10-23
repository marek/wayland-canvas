#pragma once

#include <json-glib/json-glib.h>

#include <string>
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

// Compact JSON text for a tree the caller still owns.
inline std::string node_text(JsonNode *node) {
	JsonGeneratorPtr generator(json_generator_new());
	json_generator_set_root(generator.get(), node); // copies the node
	gsize len = 0;
	GCharPtr data(json_generator_to_data(generator.get(), &len));
	return data != nullptr ? std::string(data.get(), len) : std::string{};
}

// Builds one JSON object as compact text; `build` fills it between the braces.
template <typename Build>
std::string object_text(Build &&build) {
	JsonBuilderPtr builder(json_builder_new());
	json_builder_begin_object(builder.get());
	build(builder.get());
	json_builder_end_object(builder.get());
	JsonNode *root = json_builder_get_root(builder.get()); // transfer full
	std::string text = root != nullptr ? node_text(root) : std::string{"{}"};
	json_node_unref(root);
	return text;
}

inline void add_number(JsonBuilder *builder, int value) {
	json_builder_add_int_value(builder, value);
}

inline void add_number(JsonBuilder *builder, double value) {
	json_builder_add_double_value(builder, value);
}

// The x, y, width and height members of a rectangle: the one shape every
// rectangle has on the wire.
template <typename Number>
void add_rect_members(JsonBuilder *builder, Number x, Number y, Number width, Number height) {
	json_builder_set_member_name(builder, "x");
	add_number(builder, x);
	json_builder_set_member_name(builder, "y");
	add_number(builder, y);
	json_builder_set_member_name(builder, "width");
	add_number(builder, width);
	json_builder_set_member_name(builder, "height");
	add_number(builder, height);
}

// The string a node holds; null for any other node, or none.
inline const char *node_string(JsonNode *node) {
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : nullptr;
}

// The object a node holds; null for any other node, or none.
inline JsonObject *node_object(JsonNode *node) {
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_OBJECT
		? json_node_get_object(node) : nullptr;
}

// The array a node holds; null for any other node, or none.
inline JsonArray *node_array(JsonNode *node) {
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_ARRAY
		? json_node_get_array(node) : nullptr;
}
