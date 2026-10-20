#include "core/fs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <format>

namespace fs {

bool mkdir_p(const std::string &path) {
	for (std::size_t slash = path.find('/'); ; slash = path.find('/', slash + 1)) {
		const std::string made = path.substr(0, slash == std::string::npos
			? path.size() : slash);
		if (!made.empty() && mkdir(made.c_str(), 0755) != 0 && errno != EEXIST) {
			return false;
		}
		if (slash == std::string::npos) {
			break;
		}
	}
	struct stat info{};
	return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::optional<std::string> read_text_file(const std::string &path, std::size_t max_bytes) {
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		return std::nullopt;
	}
	std::string text;
	char chunk[4096];
	std::size_t n = 0;
	while ((n = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
		if (n > max_bytes - text.size()) {
			std::fclose(file);
			return std::nullopt;
		}
		text.append(chunk, n);
	}
	const bool failed = std::ferror(file) != 0;
	std::fclose(file);
	if (failed) {
		return std::nullopt;
	}
	return text;
}

JsonParserPtr read_json_file(const std::string &path) {
	const std::optional<std::string> text = read_text_file(path);
	return text.has_value() ? parse_json(*text) : nullptr;
}

bool write_text_atomic(const std::string &path, std::string_view text) {
	const std::string tmp = std::format("{}.tmp-{}", path, getpid());
	std::FILE *file = std::fopen(tmp.c_str(), "wb");
	if (file == nullptr) {
		return false;
	}
	bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), file) == text.size();
	// fclose flushes: its result is the write's last chance to report an error
	ok = std::fclose(file) == 0 && ok;
	if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

} // namespace fs
