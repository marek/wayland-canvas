#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "core/settings.h"
#include "core/util.h"

class Server;

// canvas.conf (core/settings.h): the file's text as last read, the settings in
// force and the lines it ignored. `load` applies them to the subsystems that
// read them; `write` replaces the file atomically and applies it at once
// (docs/intents/input/config-file.md).
class ConfStore {
public:
	explicit ConfStore(Server &server);
	~ConfStore();
	ConfStore(const ConfStore &) = delete;
	ConfStore &operator=(const ConfStore &) = delete;

	// Writes the file with every default when it is missing (its directory
	// created with it), watches it — a save or a delete re-reads it and
	// applies to the running session — and reads it. Needs the event loop.
	void init();
	// Stops the watch; the loop must still be there.
	void finish();

	const std::string &path() const { return path_; }
	const std::string &text() const { return text_; }
	const std::vector<settings::Error> &errors() const { return errors_; }

	// The settings in force.
	settings::Resolved resolved;

	// Reads the file and applies it to the running session: the bindings, the
	// gesture options and, when the file has lines it ignored, the hint that
	// names the first on every screen. The first load never emits the settings
	// event; later ones only when something changed, so the watch's own echo of
	// a write is silent.
	void load(bool first);
	// Writes `new_text` as canvas.conf — beside the old file, then renamed
	// over it, so neither an editor nor a crash sees half a file — and
	// applies it at once. False, logged, when the file cannot be written: the
	// running settings stay.
	bool write(const std::string &new_text);

private:
	void write_defaults(const std::string &dir);
	void watch(const std::string &dir);
	// Reads the inotify events of the file's directory and re-reads the file
	// when it was saved or deleted.
	static int handle_change(int fd, uint32_t mask, void *data);

	Server &server_;
	std::string path_;
	std::string text_;
	std::vector<settings::Error> errors_;
	int inotify_fd_ = -1;
	EventSourcePtr source_;
};
