#pragma once

// Scratch space and small file helpers. Everything a scenario writes goes
// under $CANVAS_TMP (the repo's git-ignored tmp/ when it is unset).

#include <optional>
#include <string>
#include <string_view>

namespace harness {

// $CANVAS_TMP, or "tmp".
std::string tmp_root();

// The scenario's own scratch directory, <tmp_root>/scenarios/<scenario>,
// made empty: an earlier run's files are never mistaken for this one's.
const std::string &make_scratch(std::string_view scenario);
// The directory make_scratch made.
const std::string &scratch();
// <scratch>/<relative>, created: a place of the scenario's own.
std::string scratch_dir(std::string_view relative);

// Creates `path` and its parents; an existing directory is fine.
void mkdir_p(const std::string &path);

// Removes whatever is at `path`, then creates it empty.
void fresh_dir(const std::string &path);

std::optional<std::string> read_file(const std::string &path);
// The file's text, empty when it is missing.
std::string read_text(const std::string &path);
bool write_file(const std::string &path, std::string_view text);
// Writes an executable script.
bool write_script(const std::string &path, std::string_view text);
bool file_exists(const std::string &path);

// How many times `word` occurs in `text`.
int count_of(std::string_view text, std::string_view word);

} // namespace harness
