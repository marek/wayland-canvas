#pragma once

#include <optional>
#include <string>

#include "core/theme.h"

namespace theme_prefs {

// Sets the applications' light/dark preference (org.gnome.desktop.interface
// color-scheme, shared by every session) to match `mode`, only when
// `real_session`: the compositor opened a seat session from a TTY. A nested or
// headless run must never write it; the caller decides, so the rule is testable.
void apply(ThemeMode mode, bool real_session);

// The user's icon theme, org.gnome.desktop.interface icon-theme, read
// through gio in this process: the compositor's loop never waits on a
// program. Unset when the desktop ships no such schema or the key is empty.
std::optional<std::string> icon_theme();

}
