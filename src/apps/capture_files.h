#pragma once

#include <glib.h>

#include <string>

// The XDG user dir, from user-dirs.dirs through glib, with `~/fallback` when
// the user names none.
std::string user_dir(GUserDirectory which, const char *fallback);

// Creates `path` and every missing parent; an existing dir is fine, and
// whatever else is wrong shows when the file is written.
void mkdir_p(const std::string &path);

// The local-time name part every shot and recording carries.
std::string timestamp();
