#pragma once

class IpcServer;
class Server;

// The methods of the control socket (docs/intents/platform/ipc.md), one
// function per area, in that area's file `ipc_methods_<area>.cpp`: each
// registers the methods it owns, so a new method is one `register_method`
// call there. A new area adds its function here and one line to
// register_ipc_methods.
void register_state_methods(IpcServer &ipc, Server &server); // state, theme.set
// window.focus, overview.toggle and launcher.toggle
void register_window_methods(IpcServer &ipc, Server &server);
// settings.get, settings.set, shortcut.capture and shortcut.cancel
void register_settings_methods(IpcServer &ipc, Server &server);
void register_capture_methods(IpcServer &ipc, Server &server); // capture.pick, capture.stop
// surface.place, surface.unplace, surface.move and surface.resize
void register_surface_methods(IpcServer &ipc, Server &server);
void register_action_methods(IpcServer &ipc, Server &server); // action.run

void register_ipc_methods(IpcServer &ipc, Server &server);
