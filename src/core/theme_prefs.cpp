#include "core/theme_prefs.h"

#include <cstdio>
#include <format>
#include <print>
#include <gio/gio.h>

namespace theme_prefs {

namespace {

constexpr const char *schema_id = "org.gnome.desktop.interface";
constexpr const char *key = "color-scheme";

} // namespace

void apply(ThemeMode mode, bool real_session) {
	if (!real_session) {
		return; // nested or headless: the preference is not this run's to set
	}
	// The schema ships with the desktop (gsettings-desktop-schemas); without
	// it there is nothing to set and the session carries on untouched.
	GSettingsSchemaSource *source = g_settings_schema_source_get_default();
	GSettingsSchema *schema = source != nullptr
		? g_settings_schema_source_lookup(source, schema_id, true) : nullptr;
	if (schema == nullptr) {
		std::println(stderr, "wayland-canvas: no {} schema: the applications' "
			"preference was not set", schema_id);
		return;
	}
	GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
	const char *value = mode == ThemeMode::light ? "prefer-light" : "prefer-dark";
	if (!g_settings_set_string(settings, key, value)) {
		std::println(stderr, "wayland-canvas: {} {} is not writable: the "
			"applications' preference was not set", schema_id, key);
	}
	g_settings_sync(); // the keyfile backend writes its store out on sync
	g_object_unref(settings);
	g_settings_schema_unref(schema);
}

} // namespace theme_prefs
