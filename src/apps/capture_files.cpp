#include "apps/capture_files.h"

#include <sys/stat.h>

#include <cstdlib>
#include <ctime>

namespace {

std::string home_dir() {
	const char *home = std::getenv("HOME");
	return home != nullptr && *home != '\0' ? std::string(home) : std::string{"."};
}

} // namespace

std::string user_dir(GUserDirectory which, const char *fallback) {
	const char *dir = g_get_user_special_dir(which);
	if (dir != nullptr && *dir != '\0') {
		return dir;
	}
	return home_dir() + "/" + fallback;
}

void mkdir_p(const std::string &path) {
	std::string built;
	for (std::size_t i = 0; i < path.size(); i++) {
		built += path[i];
		if (path[i] == '/' || i + 1 == path.size()) {
			mkdir(built.c_str(), 0755); // EEXIST is fine; the write reports the rest
		}
	}
}

std::string timestamp() {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char buf[40];
	std::strftime(buf, sizeof buf, "%Y-%m-%d at %H.%M.%S", &local);
	return buf;
}
