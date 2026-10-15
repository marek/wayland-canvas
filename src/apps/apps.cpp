#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "apps/apps.h"
#include "core/config.h"
#include "core/server.h"

namespace {

constexpr std::size_t max_file = 256 * 1024;
constexpr int max_depth = 4;

std::string path_join(std::string_view a, std::string_view b) {
	while (!a.empty() && a.back() == '/') {
		a.remove_suffix(1);
	}
	std::string s;
	s.reserve(a.size() + b.size() + 1);
	s += a;
	s += '/';
	s += b;
	return s;
}

// ---- scanning state (load only) ----

struct Scan {
	std::vector<App> items;
	std::vector<std::string> seen; // desktop ids already handled, listed or not
	std::string_view lang;
};

bool id_seen(const Scan &sc, std::string_view id) {
	for (const std::string &seen : sc.seen) {
		if (seen == id) {
			return true;
		}
	}
	return false;
}

std::string read_file(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		return {};
	}
	f.seekg(0, std::ios::end);
	const std::streamoff sz = f.tellg();
	if (sz < 0 || sz > static_cast<std::streamoff>(max_file)) {
		return {};
	}
	f.seekg(0, std::ios::beg);
	std::string buf(static_cast<std::size_t>(sz), '\0');
	f.read(buf.data(), sz);
	if (f.gcount() != sz) {
		return {};
	}
	return buf;
}

// TryExec present? Absolute path: executable file. Else: on $PATH.
bool try_exec_ok(const std::string &te) {
	if (te.empty()) {
		return false;
	}
	if (te[0] == '/') {
		return access(te.c_str(), X_OK) == 0;
	}
	const char *path = std::getenv("PATH");
	if (path == nullptr || *path == '\0') {
		return false;
	}
	std::string_view rest = path;
	while (!rest.empty()) {
		const std::size_t colon = rest.find(':');
		const std::string_view dir = colon == std::string_view::npos
			? rest : rest.substr(0, colon);
		if (!dir.empty()) {
			const std::string cand = path_join(dir, te);
			if (access(cand.c_str(), X_OK) == 0) {
				return true;
			}
		}
		if (colon == std::string_view::npos) {
			break;
		}
		rest = rest.substr(colon + 1);
	}
	return false;
}

// `base` is the applications root used for desktop-id computation; `depth`
// 0 is the applications dir itself.
void scan_dir(Scan &sc, const std::string &base, const std::string &dir, int depth) {
	DIR *d = opendir(dir.c_str());
	if (d == nullptr) {
		return;
	}
	while (struct dirent *ent = readdir(d)) {
		std::string_view name = ent->d_name;
		if (name == "." || name == "..") {
			continue;
		}
		std::string full = path_join(dir, name);
		struct stat st{};
		if (stat(full.c_str(), &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < max_depth) {
				scan_dir(sc, base, full, depth + 1);
			}
			continue;
		}
		if (!S_ISREG(st.st_mode) || !name.ends_with(".desktop")) {
			continue;
		}

		// desktop id: path below the applications dir, '/' -> '-'
		std::string id = full.substr(base.size());
		while (!id.empty() && id.front() == '/') {
			id.erase(0, 1);
		}
		for (char &c : id) {
			if (c == '/') {
				c = '-';
			}
		}
		// the first occurrence of an id wins, even when it is hidden
		if (id_seen(sc, id)) {
			continue;
		}

		std::string contents = read_file(full);
		if (std::optional<App> app = App::parse(contents, id, sc.lang)) {
			if (app->try_exec.empty() || try_exec_ok(app->try_exec)) {
				sc.items.push_back(std::move(*app));
			}
		}
		sc.seen.push_back(std::move(id));
	}
	closedir(d);
}

// $XDG_DATA_HOME/applications, then each $XDG_DATA_DIRS/applications.
std::vector<std::string> application_dirs() {
	std::vector<std::string> dirs;

	std::string data_home;
	if (const char *x = std::getenv("XDG_DATA_HOME"); x != nullptr && *x != '\0') {
		data_home = x;
	} else if (const char *home = std::getenv("HOME"); home != nullptr) {
		data_home = path_join(home, ".local/share");
	}
	if (!data_home.empty()) {
		dirs.push_back(path_join(data_home, "applications"));
	}

	const char *dd = std::getenv("XDG_DATA_DIRS");
	if (dd == nullptr || *dd == '\0') {
		dd = "/usr/local/share:/usr/share";
	}
	std::string_view rest = dd;
	while (!rest.empty()) {
		const std::size_t colon = rest.find(':');
		const std::string_view base = colon == std::string_view::npos
			? rest : rest.substr(0, colon);
		if (!base.empty()) {
			dirs.push_back(path_join(base, "applications"));
		}
		if (colon == std::string_view::npos) {
			break;
		}
		rest = rest.substr(colon + 1);
	}
	return dirs;
}

} // namespace

void Apps::load() {
	items_.clear();

	std::string_view lang;
	if (const char *locale = std::getenv("LC_ALL"); locale != nullptr && *locale != '\0') {
		lang = locale;
	} else if (locale = std::getenv("LC_MESSAGES"); locale != nullptr && *locale != '\0') {
		lang = locale;
	} else if (locale = std::getenv("LANG"); locale != nullptr && *locale != '\0') {
		lang = locale;
	}

	Scan sc;
	sc.lang = lang;
	for (const std::string &dir : application_dirs()) {
		scan_dir(sc, dir, dir, 0);
	}

	std::sort(sc.items.begin(), sc.items.end(), [](const App &a, const App &b) {
		int c = strcasecmp(a.name.c_str(), b.name.c_str());
		if (c != 0) {
			return c < 0;
		}
		return strcasecmp(a.id.c_str(), b.id.c_str()) < 0;
	});
	items_ = std::move(sc.items);
}

void Apps::clear() {
	items_.clear();
}

// ---- search ----

std::vector<std::size_t> Apps::search(std::string_view query) const {
	if (query.empty()) {
		std::vector<std::size_t> out(items_.size());
		for (std::size_t i = 0; i < out.size(); i++) {
			out[i] = i;
		}
		return out;
	}

	struct Hit {
		std::size_t idx;
		int score;
	};
	std::vector<Hit> hits;
	for (std::size_t i = 0; i < items_.size(); i++) {
		int score = items_[i].match(query);
		if (score >= 0) {
			hits.push_back({.idx = i, .score = score});
		}
	}

	std::sort(hits.begin(), hits.end(), [this](const Hit &x, const Hit &y) {
		if (x.score != y.score) {
			return x.score > y.score; // better match first
		}
		int c = strcasecmp(items_[x.idx].name.c_str(), items_[y.idx].name.c_str());
		if (c != 0) {
			return c < 0;
		}
		return x.idx < y.idx;
	});

	std::vector<std::size_t> out;
	out.reserve(hits.size());
	for (const Hit &h : hits) {
		out.push_back(h.idx);
	}
	return out;
}

// ---- launching ----

void launch(const App &app) {
	if (app.exec.empty()) {
		return;
	}
	std::string cmd = app.terminal
		? std::format("{} -e {}", config::terminal_cmd, app.exec)
		: std::format("exec {}", app.exec);
	spawn(cmd.c_str());
}
