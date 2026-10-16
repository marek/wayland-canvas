#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>
#include <sys/types.h>
#include "apps/desktop_entry.h"

// Installed applications.
class Apps {
public:
	// Scans $XDG_DATA_HOME/applications then each $XDG_DATA_DIRS/applications (defaults
	// ~/.local/share, /usr/local/share:/usr/share); the first file with a given desktop id
	// wins, subdirectories map to "dir-name.desktop" ids, and entries whose TryExec binary
	// isn't on $PATH are dropped. Sorted by name, case-insensitively. Replaces the list.
	void load();
	void clear();
	std::span<const App> items() const { return items_; }

	// Indices into items(), best match first (ties alphabetical); an empty query lists all.
	std::vector<std::size_t> search(std::string_view query) const;

private:
	std::vector<App> items_;
};

// Runs the app via spawn(); Terminal=true apps run inside $TERMINAL -e.
// Returns the launched shell's pid, or -1 (no Exec, or the spawn failed).
pid_t launch(const App &app);
