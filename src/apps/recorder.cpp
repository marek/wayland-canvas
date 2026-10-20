#include "apps/recorder.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <format>
#include <string>
#include <vector>

#include "apps/capture_files.h"
#include "core/config.h"
#include "core/fs.h"
#include "core/server.h"
#include "platform/output.h"
#include "render/capture.h"

Recorder::Recorder(Server &server)
	: server_(server) {
}

Recorder::~Recorder() {
	finish();
}

void Recorder::start(Output &output, const std::optional<wlr_box> &area) {
	if (running()) {
		server_.hint.set("Already recording — Alt+Print stops it");
		return;
	}
	wlr_box target = area.value_or(wlr_box{0, 0, output.width, output.height});
	wlr_box bounds{0, 0, output.width, output.height};
	wlr_box clipped{};
	wlr_box_intersection(&clipped, &target, &bounds);
	// The frames come from the offscreen captures, so what the recording
	// shows is what a shot of the same area would: the screen's content,
	// none of the compositor's own UI over it.
	wlr_box dev = output_device_size(output, clipped);
	if (wlr_box_empty(&clipped) || wlr_box_empty(&dev)) {
		server_.hint.set("Nothing there to record");
		return;
	}
	buffer_.reset(make_capture_buffer(server_, dev.width, dev.height));
	if (buffer_ == nullptr) {
		wlr_log(WLR_ERROR, "capture: no buffer to record into");
		server_.hint.set("The recording could not start");
		return;
	}
	std::string dir = user_dir(G_USER_DIRECTORY_VIDEOS, "Videos");
	fs::mkdir_p(dir);
	path_ = std::format("{}/Screen Recording {}.mp4", dir, timestamp());

	// ffmpeg reads raw BGRA frames from the pipe and does the encoding;
	// the crop keeps odd-sized areas encodable as yuv420p.
	std::vector<std::string> args{config::capture_recorder, "-hide_banner",
		"-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra",
		"-s", std::format("{}x{}", dev.width, dev.height),
		"-r", std::format("{}", config::capture_recorder_fps),
		"-i", "pipe:0", "-vf", "crop=trunc(iw/2)*2:trunc(ih/2)*2",
		"-c:v", "libx264", "-preset", "veryfast", "-crf", "23",
		"-pix_fmt", "yuv420p", "-movflags", "+faststart", path_};
	std::vector<char *> argv;
	argv.reserve(args.size() + 1);
	for (const std::string &arg : args) {
		argv.push_back(const_cast<char *>(arg.c_str()));
	}
	argv.push_back(nullptr);

	// Only the write end is non-blocking: the encoder reads its own end
	// the ordinary way.
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		wlr_log_errno(WLR_ERROR, "capture: could not open the recorder pipe");
		release_frames();
		path_.clear();
		server_.hint.set("The recording could not start");
		return;
	}
	fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t mask;
		sigemptyset(&mask);
		sigprocmask(SIG_SETMASK, &mask, nullptr);
		std::signal(SIGINT, SIG_DFL);
		std::signal(SIGTERM, SIG_DFL);
		std::signal(SIGPIPE, SIG_DFL);
		dup2(fds[0], STDIN_FILENO);
		close(fds[0]);
		close(fds[1]);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
			if (null > STDERR_FILENO) {
				close(null);
			}
		}
		// ffmpeg starts with only its three standard fds: nothing the
		// compositor holds open reaches it.
		close_range(STDERR_FILENO + 1, ~0U, 0);
		execvp(argv[0], argv.data());
		_exit(127);
	}
	close(fds[0]);
	if (pid < 0) {
		close(fds[1]);
		wlr_log_errno(WLR_ERROR, "capture: could not fork the recorder");
		release_frames();
		path_.clear();
		server_.hint.set("The recording could not start");
		return;
	}
	pid_ = pid;
	fd_ = fds[1];
	output_ = &output;
	area_ = clipped;
	dropped_ = 0;
	frame_.assign(static_cast<std::size_t>(dev.width) * dev.height * 4, 0);
	sent_ = frame_.size(); // no frame waits yet

	reaper_.reset(wl_event_loop_add_timer(server_.loop,
		[](void *data) -> int { return static_cast<Recorder *>(data)->reap(); }, this));
	if (reaper_) {
		wl_event_source_timer_update(reaper_.get(), config::capture_recorder_poll_ms);
	}
	// Idle until a frame waits for room in the pipe; a dead encoder is
	// reported to it regardless.
	writer_.reset(wl_event_loop_add_fd(server_.loop, fd_, 0, writable, this));
	frame_timer_.reset(wl_event_loop_add_timer(server_.loop,
		[](void *data) -> int { return static_cast<Recorder *>(data)->tick(); }, this));
	if (frame_timer_) {
		wl_event_source_timer_update(frame_timer_.get(), 1); // the first frame now
	}
	server_.ipc.emit_recording(true, path_);
	server_.hint.set("Recording — Alt+Print or the bar's red dot stops it");
}

// One frame is due: dropped when the encoder has not taken the last one yet,
// else the area's capture into the kept buffer, written as far as the pipe
// takes it.
int Recorder::tick() {
	if (fd_ < 0 || output_ == nullptr) {
		return 0;
	}
	if (sent_ < frame_.size()) {
		dropped_++;
	} else if (capture_output(*output_, area_, buffer_.get(), frame_)) {
		sent_ = 0;
		write_pending();
	}
	if (fd_ >= 0) {
		wl_event_source_timer_update(frame_timer_.get(),
			std::max(1, 1000 / config::capture_recorder_fps));
	}
	return 0;
}

void Recorder::write_pending() {
	while (sent_ < frame_.size()) {
		ssize_t n = write(fd_, frame_.data() + sent_, frame_.size() - sent_);
		if (n > 0) {
			sent_ += static_cast<std::size_t>(n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			// The rest goes when the pipe has room again.
			wl_event_source_fd_update(writer_.get(), WL_EVENT_WRITABLE);
			return;
		} else {
			stop(); // EPIPE or nothing taken: the encoder has gone
			return;
		}
	}
	wl_event_source_fd_update(writer_.get(), 0);
}

int Recorder::writable(int, std::uint32_t mask, void *data) {
	auto *self = static_cast<Recorder *>(data);
	if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
		self->stop(); // the encoder closed its end
		return 0;
	}
	self->write_pending();
	return 0;
}

int Recorder::reap() {
	int status = 0;
	pid_t done = waitpid(pid_, &status, WNOHANG);
	if (done == 0 || (done < 0 && errno == EINTR)) {
		wl_event_source_timer_update(reaper_.get(), config::capture_recorder_poll_ms);
		return 0;
	}
	gone(done < 0 ? 0 : status);
	return 0;
}

// The encoder process has left its file finished (or failed): report it.
void Recorder::gone(int status) {
	pid_ = -1;
	writer_.reset();
	if (fd_ >= 0) {
		close(fd_);
		fd_ = -1;
	}
	reaper_.reset();
	frame_timer_.reset();
	release_frames();
	output_ = nullptr;
	std::string path = std::move(path_);
	path_.clear();
	server_.ipc.emit_recording(false, path);
	struct stat st{};
	bool saved = !path.empty() && stat(path.c_str(), &st) == 0 && st.st_size > 0;
	wlr_log(WLR_INFO, "capture: the recorder exited with status %#x; %s", status,
		path.c_str());
	if (dropped_ > 0) {
		wlr_log(WLR_INFO, "capture: %zu frames dropped, the encoder was behind", dropped_);
	}
	server_.hint.set(saved ? "Recording saved" : "The recording failed");
}

void Recorder::release_frames() {
	buffer_.reset();
	frame_ = {};
	sent_ = 0;
}

void Recorder::stop() {
	if (!running()) {
		return;
	}
	// The pipe's end is the encoder's clean stop: it sees the last of its
	// input, finalises the container and exits. A frame left half written
	// is a short last packet, which it drops.
	writer_.reset();
	if (fd_ >= 0) {
		close(fd_);
		fd_ = -1;
	}
	frame_timer_.reset();
	release_frames();
	server_.hint.set("Stopping the recording…");
}

void Recorder::output_destroyed(Output &output) {
	if (output_ == &output) {
		stop(); // the recorded screen is gone
	}
}

void Recorder::finish() {
	stop();
	if (pid_ > 0) {
		// The polite stop gets a moment, then the recording is torn down.
		for (int waited = 0; waited < config::capture_recorder_stop_ms;
				waited += config::capture_recorder_poll_ms) {
			int status = 0;
			pid_t done = waitpid(pid_, &status, WNOHANG);
			if (done == pid_ || (done < 0 && errno == ECHILD)) {
				pid_ = -1;
				break;
			}
			timespec pause{.tv_sec = 0,
				.tv_nsec = config::capture_recorder_poll_ms * 1'000'000L};
			nanosleep(&pause, nullptr);
		}
		if (pid_ > 0) {
			kill(pid_, SIGKILL);
			int status = 0;
			while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
			}
			pid_ = -1;
		}
	}
	reaper_.reset();
	output_ = nullptr;
}
