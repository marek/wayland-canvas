#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "canvas/desktop.h"
#include "core/theme.h"

class Server;
typedef struct _JsonBuilder JsonBuilder;

// One mapped window as the app bar sees it (docs/intents/shell/app-bar.md).
struct WindowRow {
	uint64_t id = 0;
	std::string app_id, title;
	std::optional<DesktopId> desktop;
	bool minimized = false, focused = false;
	bool operator==(const WindowRow &) const = default;
};

// The compositor's state as the protocol reports it
// (docs/intents/platform/ipc.md): the one set of serializers the `state`
// method, the events and `settings.get` share. Each adds one member, or
// more, to the object the builder is inside.
std::string_view theme_name(ThemeMode mode);
void add_outputs_array(JsonBuilder *builder, Server &server);
void add_desktops_array(JsonBuilder *builder, Server &server);
// Reads the open windows into `rows`, in the order they opened. The rows
// already there are overwritten in place, so a list read every frame
// allocates only while it grows.
void collect_windows(Server &server, std::vector<WindowRow> &rows);
void add_windows_array(JsonBuilder *builder, const std::vector<WindowRow> &rows);
// The settings as settings.get returns them: the file, its ignored lines,
// every action with its keys and defaults in the registry's order, the
// commands, each section's settings and what each setting takes.
void add_settings(JsonBuilder *builder, Server &server);
