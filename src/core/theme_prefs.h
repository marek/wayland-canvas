#pragma once

#include "core/theme.h"

namespace theme_prefs {

// The applications' light/dark preference — org.gnome.desktop.interface
// color-scheme, the one system-wide setting every other session shares —
// set to match `mode`, but only when this compositor is the real session
// (it opened a seat session, from a TTY). A nested run (wayland or x11
// backend) or a headless one must never write it. `real_session` is the
// compositor's own answer to that, passed in so the rule itself can be
// tested: theme_prefs.cpp holds the write, Server's caller holds the gate.
void apply(ThemeMode mode, bool real_session);

}
