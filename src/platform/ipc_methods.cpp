#include "platform/ipc_methods.h"

void register_ipc_methods(IpcServer &ipc, Server &server) {
	register_state_methods(ipc, server);
	register_window_methods(ipc, server);
	register_settings_methods(ipc, server);
	register_capture_methods(ipc, server);
	register_surface_methods(ipc, server);
	register_action_methods(ipc, server);
}
