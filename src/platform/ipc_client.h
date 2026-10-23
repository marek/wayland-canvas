#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>
#include "core/json.h"

// `wayland-canvas msg <method> [<params as JSON>]`: sends one request,
// prints the result and exits 0, or prints the error to stderr and exits 1.
// `msg subscribe` prints each event as one line until interrupted. argv[1]
// is the method; a transport failure is an error too. Used by main().
int msg_command(int argc, char *argv[]);

// The pieces of the client, shared with the portal's chooser bridge and the
// scenarios' control client.

// Connects to $WAYLAND_CANVAS_SOCKET, else to the socket of the compositor
// $WAYLAND_DISPLAY names: a service the session bus starts (the portal's
// chooser) knows only the display. -1 (explained on stderr) on failure.
int connect_to_socket();

// Blocking write of the whole buffer; false on failure.
bool write_request(int fd, std::string_view bytes);

// Blocking: one newline-terminated line, or nullopt on EOF or error.
std::optional<std::string> read_line(int fd);

// A connected socket, closed when it goes out of scope.
class OwnedFd {
public:
	explicit OwnedFd(int fd = -1) : fd_(fd) {
	}
	OwnedFd(OwnedFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {
	}
	OwnedFd(const OwnedFd &) = delete;
	OwnedFd &operator=(const OwnedFd &) = delete;
	OwnedFd &operator=(OwnedFd &&) = delete;
	~OwnedFd() {
		if (fd_ >= 0) {
			close(fd_);
		}
	}
	int get() const { return fd_; }

private:
	int fd_;
};

// One request sent and its first response read; the socket stays open for the
// events a subscription goes on to read.
struct Exchange {
	OwnedFd socket;
	JsonParserPtr parser; // owns `answer`
	JsonObject *answer = nullptr; // null when the response is not a JSON object
};

// Sends `method` with `params` (JSON text, empty for none) on a new connection
// and reads the first response. Nullopt, explained on stderr, when the
// compositor cannot be reached or does not answer.
std::optional<Exchange> round_trip(std::string_view method, std::string_view params);
