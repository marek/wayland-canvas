// Unit tests for src/core/fs.cpp and src/core/text.cpp: the file and text helpers the
// compositor and the agent usage collector share. Files live under $CANVAS_TMP/unit/fs-text.
// Run via `meson test` (test 'fs-text').

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/text.h"

namespace {

using unit::check;

std::string root;

void put(const std::string &path, std::string_view contents) {
	std::FILE *file = std::fopen(path.c_str(), "wb");
	check(file != nullptr, std::format("cannot write {}", path));
	if (file != nullptr) {
		std::fwrite(contents.data(), 1, contents.size(), file);
		std::fclose(file);
	}
}

bool is_dir(const std::string &path) {
	struct stat info{};
	return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// How many entries of `dir` have a name starting with `prefix`.
int named(const std::string &dir, std::string_view prefix) {
	int count = 0;
	for (const auto &entry : std::filesystem::directory_iterator(dir)) {
		count += entry.path().filename().string().starts_with(prefix) ? 1 : 0;
	}
	return count;
}

// What the JSON parser reads back from `quoted`, spliced into an array.
std::optional<std::string> unquoted(const std::string &quoted) {
	JsonParserPtr parser = parse_json("[" + quoted + "]");
	if (parser == nullptr) {
		return std::nullopt;
	}
	JsonNode *array = json_parser_get_root(parser.get());
	return json_array_get_string_element(json_node_get_array(array), 0);
}

void test_mkdir_p() {
	const std::string deep = root + "/mk/a/b/c";
	check(fs::mkdir_p(deep), "a missing chain is made");
	check(is_dir(deep), "the leaf exists");
	check(fs::mkdir_p(deep), "an existing directory is fine");
	check(fs::mkdir_p(deep + "/"), "a trailing slash is fine");

	put(root + "/mk/file", "x");
	check(!fs::mkdir_p(root + "/mk/file"), "a file in the way is a failure");
	check(!fs::mkdir_p(root + "/mk/file/below"), "a path below a file is a failure");
}

void test_read_text_file() {
	put(root + "/read.txt", "one\ntwo\n");
	check(fs::read_text_file(root + "/read.txt") == "one\ntwo\n", "the whole file");
	check(!fs::read_text_file(root + "/missing").has_value(), "a missing file is nullopt");
	check(fs::read_text_file(root + "/read.txt", 8).has_value(),
		"a file of exactly max_bytes reads");
	check(!fs::read_text_file(root + "/read.txt", 7).has_value(),
		"a file over max_bytes is nullopt");
	put(root + "/empty.txt", "");
	check(fs::read_text_file(root + "/empty.txt") == "", "an empty file is empty text");
	check(!fs::read_text_file(root).has_value(), "a directory is nullopt");
	std::string with_nul = "a";
	with_nul.push_back('\0');
	with_nul += "b";
	put(root + "/nul.txt", with_nul);
	check(fs::read_text_file(root + "/nul.txt") == with_nul, "bytes after a NUL are kept");
}

void test_read_json_file() {
	put(root + "/ok.json", R"({"a":1})");
	JsonParserPtr ok = fs::read_json_file(root + "/ok.json");
	check(root_object(ok) != nullptr, "a JSON file parses");
	put(root + "/bad.json", "{not json");
	check(fs::read_json_file(root + "/bad.json") == nullptr, "invalid JSON is null");
	put(root + "/empty.json", "");
	check(fs::read_json_file(root + "/empty.json") == nullptr, "an empty file is null");
	check(fs::read_json_file(root + "/missing.json") == nullptr, "a missing file is null");
}

void test_write_text_atomic() {
	const std::string path = root + "/atomic.txt";
	check(fs::write_text_atomic(path, "first"), "a new file is written");
	check(fs::read_text_file(path) == "first", "the new file holds the text");
	check(fs::write_text_atomic(path, "second, longer"), "a file is replaced");
	check(fs::read_text_file(path) == "second, longer", "the replacement holds the text");
	check(fs::write_text_atomic(path, ""), "empty text is written");
	check(fs::read_text_file(path) == "", "the empty file is empty");
	check(named(root, "atomic.txt.") == 0, "no temporary file stays beside the target");

	check(!fs::write_text_atomic(root + "/no-such-dir/file", "x"),
		"a missing directory is a failure");

	// rename cannot put a file over a directory: the write fails, the directory stays and
	// the temporary file goes
	const std::string blocked = root + "/blocked";
	fs::mkdir_p(blocked);
	check(!fs::write_text_atomic(blocked, "x"), "a directory in the way is a failure");
	check(is_dir(blocked), "the directory in the way is untouched");
	check(named(root, "blocked.") == 0, "a failed write leaves no temporary file");
}

void test_quoting() {
	check(text::shell_quote("plain") == "'plain'", "plain text is quoted");
	check(text::shell_quote("") == "''", "empty text is an empty pair");
	check(text::shell_quote("it's") == "'it'\\''s'", "a single quote is closed and escaped");
	check(text::shell_quote("a b$c\"d") == "'a b$c\"d'", "everything else is literal");

	check(text::json_quote("plain") == "\"plain\"", "plain text is quoted");
	check(text::json_quote("a\"b\\c") == R"("a\"b\\c")", "quote and backslash are escaped");
	check(text::json_quote("\b\f\n\r\t") == R"("\b\f\n\r\t")",
		"the short escapes include backspace and form feed");
	check(text::json_quote(std::string_view("\x01\x1f", 2)) == R"("\u0001\u001f")",
		"other control characters are \\u escapes");
	check(text::json_quote("é→") == "\"é→\"", "UTF-8 passes through");

	std::string every;
	for (int c = 1; c < 0x80; c++) {
		every.push_back(static_cast<char>(c));
	}
	check(unquoted(text::json_quote(every)) == every,
		"every ASCII character survives a round trip through a JSON parser");
}

void test_text_helpers() {
	check(text::ascii_lower('A') == 'a' && text::ascii_lower('z') == 'z'
			&& text::ascii_lower('[') == '[',
		"ascii_lower maps A-Z only");
	check(text::ci_equal("Name", "nAME") && !text::ci_equal("Name", "Names"), "ci_equal");
	check(text::ci_starts_with("Firefox Web", "fire") && !text::ci_starts_with("Fi", "fire"),
		"ci_starts_with");
	check(text::ci_contains("Web Browser", "BROWS") && !text::ci_contains("Web", "x"),
		"ci_contains finds a needle");
	check(!text::ci_contains("Web", ""), "an empty needle is contained in nothing");

	check(text::parse_int("42") == 42 && text::parse_int("-7") == -7, "parse_int");
	check(!text::parse_int("").has_value() && !text::parse_int("4x").has_value()
			&& !text::parse_int(" 4").has_value() && !text::parse_int("99999999999").has_value(),
		"parse_int refuses anything but a whole in-range integer");

	const std::vector<std::string> items = text::split_list(" a ; b;;\tc\t;", ';');
	check(items == std::vector<std::string>{"a", "b", "c"},
		"split_list trims and drops empty items");
	check(text::split_list("", ';').empty(), "an empty list has no items");
}

void test_ini() {
	const text::Ini ini = text::parse_ini(
		"stray=before any group\r\n"
		"# comment\n"
		"[Desktop Entry]\r\n"
		"Name = Files \r\n"
		"Name=ignored, the first wins\n"
		"Empty=\n"
		"no equals here\n"
		"Exec=nautilus %U\n"
		"[Other]\n"
		"Name=other\n"
		"[Desktop Entry]\n"
		"Name=a later group of the same name\n"
		"[broken\n"
		"Name=in an unnamed group\n");

	const text::IniGroup *entry = ini.group("Desktop Entry");
	check(entry != nullptr, "the first group of a name is found");
	if (entry != nullptr) {
		check(entry->get("Name") != nullptr && *entry->get("Name") == "Files",
			"the first value wins and spaces are trimmed");
		check(entry->get("Exec") != nullptr && *entry->get("Exec") == "nautilus %U",
			"later keys are read");
		check(entry->get("Empty") == nullptr, "a key with no value is skipped");
		check(entry->get("stray") == nullptr, "lines before the first group are skipped");
		check(entry->entries.size() == 3, "only the keys of its own header belong to it");
	}
	check(ini.group("Other") != nullptr && *ini.group("Other")->get("Name") == "other",
		"a second group is found");
	check(ini.group("[broken") == nullptr && ini.group("broken") == nullptr,
		"a header that does not close names no group");
	check(ini.group("Missing") == nullptr, "a group the file lacks is null");

	std::string with_nul = "[A]\nk=v\n";
	with_nul.push_back('\0');
	with_nul += "[B]\nk=w\n";
	const text::Ini cut = text::parse_ini(with_nul);
	check(cut.group("A") != nullptr && cut.group("B") == nullptr, "a NUL ends the file");
	check(text::parse_ini("").groups.empty(), "empty text has no groups");
}

} // namespace

int main() {
	const char *env = std::getenv("CANVAS_TMP");
	root = env != nullptr && *env != '\0' ? env : "tmp";
	root += "/unit/fs-text";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);

	test_mkdir_p();
	test_read_text_file();
	test_read_json_file();
	test_write_text_atomic();
	test_quoting();
	test_text_helpers();
	test_ini();

	std::filesystem::remove_all(root, ec);
	return unit::summary();
}
