// Unit tests for src/core/theme_prefs.cpp: the applications' preference
// write, proven against a private keyfile settings backend — and the
// nested/headless rule that nothing is written at all. The real session's
// dconf is never touched: GSETTINGS_BACKEND and XDG_CONFIG_HOME point at
// scratch under the repo's tmp/ before any GSettings use.
// Run via `meson test` (test 'theme-prefs').

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <gio/gio.h>

#include "check.h"
#include "core/theme_prefs.h"

namespace {

using unit::check;

} // namespace

int main() {
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp";
	root += "/unit/theme-prefs";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root + "/write", ec);
	std::filesystem::create_directories(root + "/idle", ec);
	// Every GSettings in this process goes to the private keyfile backend —
	// before anything else, so no write can ever reach the machine's dconf.
	setenv("GSETTINGS_BACKEND", "keyfile", 1);
	// The nested/headless rule first: it must not touch GSettings at all, so
	// the backend's store under a fresh XDG_CONFIG_HOME stays absent.
	setenv("XDG_CONFIG_HOME", (root + "/idle").c_str(), 1);
	theme_prefs::apply(ThemeMode::dark, false);
	check(!std::filesystem::exists(root + "/idle/glib-2.0/settings/keyfile"),
		"a not-real session wrote no settings store");

	// The real-session write: prefer-light lands in the private backend.
	setenv("XDG_CONFIG_HOME", (root + "/write").c_str(), 1);
	theme_prefs::apply(ThemeMode::light, true);
	check(std::filesystem::exists(root + "/write/glib-2.0/settings/keyfile"),
		"the real-session write created the settings store");
	GSettingsSchemaSource *source = g_settings_schema_source_get_default();
	GSettingsSchema *schema = g_settings_schema_source_lookup(source,
		"org.gnome.desktop.interface", true);
	check(schema != nullptr, "the desktop's interface schema is installed");
	if (schema != nullptr) {
		GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
		gchar *read = g_settings_get_string(settings, "color-scheme");
		check(std::string(read) == "prefer-light",
			"the real-session write set color-scheme to prefer-light");
		g_free(read);
		// The compositor's icon lookup reads the same schema without a child
		// process: what the user's settings hold is what it gets.
		check(g_settings_set_string(settings, "icon-theme", "Papirus"),
			"icon-theme is writable in the private backend");
		g_settings_sync();
		check(theme_prefs::icon_theme() == std::optional<std::string>{"Papirus"},
			"icon_theme reads the icon-theme key through gio");
		g_object_unref(settings);
		g_settings_schema_unref(schema);
	}

	return unit::summary();
}
