#include "core/conf_store.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <sys/inotify.h>
#include <unistd.h>

#include "core/fs.h"
#include "core/server.h"

namespace {

// $XDG_CONFIG_HOME when absolute, else ~/.config.
std::string config_home() {
	if (const char *xdg = getenv("XDG_CONFIG_HOME");
			xdg != nullptr && xdg[0] == '/') {
		return xdg;
	}
	if (const char *home = getenv("HOME"); home != nullptr && home[0] == '/') {
		return std::string(home) + "/.config";
	}
	return {};
}

// The gesture code's own options from the settings in force.
GestureOptions gesture_options(const settings::Resolved &conf) {
	return {
		.swipes = conf.swipes,
		.pinch_scale = conf.pinch_scale,
		.overview_down = conf.overview_down,
		.hops_away = conf.hops_away,
	};
}

} // namespace

ConfStore::ConfStore(Server &server) : server_(server) {}

ConfStore::~ConfStore() {
	finish();
}

void ConfStore::init() {
	std::string home = config_home();
	std::string dir = home + "/wayland-canvas";
	path_ = dir + "/canvas.conf";
	if (home.empty()) {
		wlr_log(WLR_ERROR, "neither $XDG_CONFIG_HOME nor $HOME is set: "
			"canvas.conf is unreadable, the defaults apply");
	} else {
		write_defaults(dir);
		watch(dir);
	}
	load(true);
}

void ConfStore::finish() {
	source_.reset();
	if (inotify_fd_ >= 0) {
		close(inotify_fd_);
		inotify_fd_ = -1;
	}
}

void ConfStore::write_defaults(const std::string &dir) {
	if (access(path_.c_str(), F_OK) == 0) {
		return;
	}
	fs::mkdir_p(dir);
	if (!fs::write_text_atomic(path_, settings::template_text())) {
		wlr_log(WLR_ERROR, "could not write %s: the defaults apply", path_.c_str());
	}
}

void ConfStore::watch(const std::string &dir) {
	inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd_ < 0 || inotify_add_watch(inotify_fd_, dir.c_str(),
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE) < 0) {
		wlr_log(WLR_ERROR, "could not watch %s for changes", path_.c_str());
		finish();
		return;
	}
	source_.reset(wl_event_loop_add_fd(server_.loop, inotify_fd_,
		WL_EVENT_READABLE, handle_change, this));
}

void ConfStore::load(bool first) {
	// a missing or unreadable file reads as empty: every default applies
	// (docs/intents/input/config-file.md)
	std::string file_text = fs::read_text_file(path_).value_or("");
	settings::Parsed parsed = settings::parse(file_text);
	settings::Resolved loaded = settings::resolve(parsed);
	bool differs = !first && !(loaded == resolved);
	resolved = std::move(loaded);
	text_ = std::move(file_text);
	errors_ = std::move(parsed.errors);
	server_.bindings.apply(resolved);
	server_.gestures.set_options(gesture_options(resolved));
	server_.input.settings_changed();
	server_.lock.settings_changed();
	server_.night_light.settings_changed();
	if (!errors_.empty()) {
		for (const settings::Error &error : errors_) {
			wlr_log(WLR_ERROR, "canvas.conf line %d: %s", error.line,
				error.text.c_str());
		}
		const settings::Error &first_error = errors_.front();
		std::string hint_text = std::format("canvas.conf line {}: {}",
			first_error.line, first_error.text);
		if (errors_.size() > 1) {
			hint_text += std::format(" (+{} more)", errors_.size() - 1);
		}
		server_.hint.set(hint_text);
	}
	if (differs) {
		server_.changed(Change::settings);
	}
}

bool ConfStore::write(const std::string &new_text) {
	if (!fs::write_text_atomic(path_, new_text)) {
		wlr_log(WLR_ERROR, "could not write %s: the running settings stay",
			path_.c_str());
		return false;
	}
	load(false); // the settings event leaves with it; the watch echoes
	return true;
}

int ConfStore::handle_change(int fd, uint32_t mask, void *data) {
	auto *store = static_cast<ConfStore *>(data);
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	// Read every event pending: a save of canvas.conf — written in place
	// or renamed over, as editors do — or its deletion re-reads it.
	alignas(struct inotify_event) char buf[4096];
	bool reload = false;
	ssize_t n = 0;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		char *at = buf;
		while (at + sizeof(struct inotify_event) <= buf + n) {
			auto *event = reinterpret_cast<struct inotify_event *>(at);
			at += sizeof(struct inotify_event) + event->len;
			if (event->len > 0 && strcmp(event->name, "canvas.conf") == 0) {
				reload = true;
			}
		}
	}
	if (reload) {
		store->load(false);
	}
	return 0;
}
