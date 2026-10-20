#pragma once

#include <glib.h>

#include <string>

// The XDG user dir, from user-dirs.dirs through glib, with `~/fallback` when
// the user names none.
std::string user_dir(GUserDirectory which, const char *fallback);

// The local-time name part every shot and recording carries.
std::string timestamp();
