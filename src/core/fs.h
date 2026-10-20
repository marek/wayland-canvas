#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "core/json.h"

// File helpers without wlroots, linked by the compositor and by the agent usage collector.
namespace fs {

// Creates `path` and every missing parent. False when a component cannot be made or `path`
// is not a directory afterwards.
bool mkdir_p(const std::string &path);

// The whole file; nullopt when it cannot be read or is longer than `max_bytes`.
std::optional<std::string> read_text_file(const std::string &path,
	std::size_t max_bytes = std::numeric_limits<std::size_t>::max());

// The file as a JSON document; null when it cannot be read or does not parse.
JsonParserPtr read_json_file(const std::string &path);

// Writes `text` to a temporary file beside `path` and renames it over `path`, so a reader
// never sees half a file. False, with `path` untouched, when any step fails.
bool write_text_atomic(const std::string &path, std::string_view text);

} // namespace fs
