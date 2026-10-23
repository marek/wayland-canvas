#include "harness/files.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <sys/stat.h>

namespace harness {

namespace {

std::string scratch_path;

} // namespace

std::string tmp_root() {
	const char *env = std::getenv("CANVAS_TMP");
	return env != nullptr && *env != '\0' ? env : "tmp";
}

const std::string &make_scratch(std::string_view scenario) {
	scratch_path = std::format("{}/scenarios/{}", tmp_root(), scenario);
	fresh_dir(scratch_path);
	return scratch_path;
}

const std::string &scratch() {
	return scratch_path;
}

std::string scratch_dir(std::string_view relative) {
	std::string path = std::format("{}/{}", scratch_path, relative);
	mkdir_p(path);
	return path;
}

void mkdir_p(const std::string &path) {
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
	if (ec) {
		std::fprintf(stderr, "mkdir %s: %s\n", path.c_str(), ec.message().c_str());
	}
}

void fresh_dir(const std::string &path) {
	std::error_code ec;
	std::filesystem::remove_all(path, ec);
	mkdir_p(path);
}

std::optional<std::string> read_file(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return std::nullopt;
	}
	std::string text;
	char chunk[4096];
	std::size_t n = 0;
	while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
		text.append(chunk, n);
	}
	std::fclose(f);
	return text;
}

std::string read_text(const std::string &path) {
	return read_file(path).value_or("");
}

bool write_file(const std::string &path, std::string_view text) {
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr) {
		std::fprintf(stderr, "cannot write %s\n", path.c_str());
		return false;
	}
	bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	return std::fclose(f) == 0 && ok;
}

bool write_script(const std::string &path, std::string_view text) {
	return write_file(path, text) && chmod(path.c_str(), 0755) == 0;
}

bool file_exists(const std::string &path) {
	struct stat st{};
	return stat(path.c_str(), &st) == 0;
}

int count_of(std::string_view text, std::string_view word) {
	int count = 0;
	for (std::size_t at = text.find(word); at != std::string_view::npos;
			at = text.find(word, at + 1)) {
		count++;
	}
	return count;
}

} // namespace harness
