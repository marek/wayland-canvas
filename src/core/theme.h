#pragma once

#include "core/config.h"

// The desktop's colour mode, switched by the `theme.set` IPC method and
// followed by everything the compositor draws.
enum class ThemeMode { dark, light };

// One per compositor: the active palette. Switching is Server::set_theme(),
// which drops every texture cache drawn with the old palette and damages
// every output; drawing code reads colors() and never caches across a switch.
class Theme {
public:
	ThemeMode mode() const { return mode_; }
	const config::Palette &colors() const {
		return mode_ == ThemeMode::dark ? config::palette_dark : config::palette_light;
	}
	void set_mode(ThemeMode mode) { mode_ = mode; }

private:
	ThemeMode mode_ = ThemeMode::dark; // the compositor starts dark until told
};
