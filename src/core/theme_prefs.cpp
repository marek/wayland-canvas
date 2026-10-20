#include "core/theme_prefs.h"

#include <cstdio>
#include <format>
#include <memory>
#include <print>
#include <gio/gio.h>

namespace theme_prefs {

namespace {

constexpr const char *schema_id = "org.gnome.desktop.interface";
constexpr const char *scheme_key = "color-scheme";
constexpr const char *icon_theme_key = "icon-theme";

using SchemaPtr = std::unique_ptr<GSettingsSchema, decltype(&g_settings_schema_unref)>;
using SettingsPtr = std::unique_ptr<GSettings, decltype(&g_object_unref)>;
using TextPtr = std::unique_ptr<gchar, decltype(&g_free)>;

// The interface schema, null when the desktop does not ship it.
SchemaPtr interface_schema() {
	GSettingsSchemaSource *source = g_settings_schema_source_get_default();
	return SchemaPtr{source != nullptr
			? g_settings_schema_source_lookup(source, schema_id, true) : nullptr,
		g_settings_schema_unref};
}

} // namespace

void apply(ThemeMode mode, bool real_session) {
	if (!real_session) {
		return; // nested or headless: the preference is not this run's to set
	}
	// The schema ships with the desktop (gsettings-desktop-schemas); without
	// it there is nothing to set and the session carries on untouched.
	SchemaPtr schema = interface_schema();
	if (schema == nullptr) {
		std::println(stderr, "wayland-canvas: no {} schema: the applications' "
			"preference was not set", schema_id);
		return;
	}
	SettingsPtr settings{g_settings_new_full(schema.get(), nullptr, nullptr), g_object_unref};
	const char *value = mode == ThemeMode::light ? "prefer-light" : "prefer-dark";
	if (!g_settings_set_string(settings.get(), scheme_key, value)) {
		std::println(stderr, "wayland-canvas: {} {} is not writable: the "
			"applications' preference was not set", schema_id, scheme_key);
	}
	g_settings_sync(); // the keyfile backend writes its store out on sync
}

std::optional<std::string> icon_theme() {
	SchemaPtr schema = interface_schema();
	if (schema == nullptr || !g_settings_schema_has_key(schema.get(), icon_theme_key)) {
		return std::nullopt;
	}
	SettingsPtr settings{g_settings_new_full(schema.get(), nullptr, nullptr), g_object_unref};
	TextPtr name{g_settings_get_string(settings.get(), icon_theme_key), g_free};
	if (name == nullptr || name.get()[0] == '\0') {
		return std::nullopt;
	}
	return std::string{name.get()};
}

} // namespace theme_prefs
