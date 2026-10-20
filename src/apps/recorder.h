#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/util.h"

class Output;
class Server;

// A screen recording: offscreen captures of one area of an output, piped as
// raw BGRA frames into the stdin of ffmpeg, the one encoder, so it works
// nested and from a TTY alike. The compositor never waits for the encoder:
// the pipe is non-blocking, a frame that does not fit waits for room in it,
// and a frame that comes due while the last is still waiting is dropped.
class Recorder {
public:
	explicit Recorder(Server &server);
	~Recorder();
	Recorder(const Recorder &) = delete;
	Recorder &operator=(const Recorder &) = delete;

	// True from the start until the encoder has left its file finished.
	bool running() const { return pid_ > 0; }
	const std::string &path() const { return path_; }
	// The frames the running recording (or else the last one) dropped
	// because the encoder had not taken the one before.
	std::size_t dropped_frames() const { return dropped_; }

	// Records `area` (output-local logical px; the whole output when empty)
	// of `output` into a file in the Videos dir, and tells the user when
	// nothing starts.
	void start(Output &output, const std::optional<wlr_box> &area);
	// Ends the running recording: the encoder is told the input is over and
	// finishes its file, and the recording event fires once it has. A no-op
	// when none runs.
	void stop();
	// An output went away: its recording stops.
	void output_destroyed(Output &output);
	// Stops the recorder and waits for it before the display goes.
	void finish();

private:
	int tick(); // one frame is due
	int reap(); // polls the encoder process while it runs
	static int writable(int fd, std::uint32_t mask, void *data);
	void write_pending(); // as much of the pending frame as the pipe takes
	void gone(int status); // the encoder process has exited
	void release_frames();

	Server &server_;

	pid_t pid_ = -1;
	int fd_ = -1; // the pipe's write end, non-blocking
	std::string path_;
	Output *output_ = nullptr;
	wlr_box area_{}; // what is recorded, output-local logical
	std::size_t dropped_ = 0;

	// One render target and one frame of pixels, kept for the whole
	// recording; `sent_` is how much of the frame the pipe has taken.
	Handle<wlr_buffer, wlr_buffer_drop> buffer_;
	std::vector<std::uint8_t> frame_;
	std::size_t sent_ = 0;

	EventSourcePtr frame_timer_;
	EventSourcePtr writer_; // writable-watch on fd_, armed while a frame waits
	EventSourcePtr reaper_;
};
