#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "platform/ipc_state.h"

class IpcServer;
class Server;

// The events the control socket's subscribers receive
// (docs/intents/platform/ipc.md). The owner of a fact flags its change
// (`Server::changed`) and `flush` tells the subscribers once per frame, in
// one place: no domain code calls the socket.
class IpcEvents {
public:
	IpcEvents(Server &server, IpcServer &ipc);

	// Reads and clears the flags, serializes what changed and pushes one event
	// per change. With no subscriber the flags are only cleared.
	void flush();
	// An `action` event: the host runs the named plugin action. `args_json` is
	// a whole JSON object's text.
	void emit_action(std::string_view name, std::string_view args_json);

private:
	// The window list is told only when it reads differently from the list
	// last pushed (docs/intents/shell/app-bar.md).
	void flush_windows();
	void flush_overview();
	void flush_recording();

	Server &server_;
	IpcServer &ipc_;
	// The window list as it was last pushed, and the rows each flush reads
	// the list into: an identical list is no event. Both keep their rows'
	// strings between frames, so comparing allocates nothing. Unset while no
	// subscriber has heard a list, which is also what a list nobody listened
	// for leaves behind.
	std::vector<WindowRow> told_rows_, window_rows_;
	bool windows_told_ = false;
	// The file the recording last told writes: a stop names it too.
	std::string recording_path_;
};
