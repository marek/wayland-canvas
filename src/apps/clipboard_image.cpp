#include "apps/clipboard_image.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/server.h"

// wlroots' own initializer for compositor-owned data sources: it lives in
// the private types/ header but is an exported symbol, and there is no
// public constructor for a source the compositor itself offers.
void wlr_data_source_init(wlr_data_source *source, const wlr_data_source_impl *impl);

struct ClipboardImage::Source {
	wlr_data_source base{};
	wl_event_loop *loop = nullptr;
	std::vector<std::uint8_t> bytes;
	std::size_t sent = 0;
	int fd = -1;
	EventSourcePtr pump;
	Listener destroyed_;

	static void send(wlr_data_source *source, const char *mime_type, int32_t fd);
	static void destroy(wlr_data_source *source);
	static int pump_write(int fd, uint32_t mask, void *data);

	void start(int32_t fd) {
		if (this->fd >= 0) {
			close(this->fd); // one transfer at a time: the newest request wins
			pump.reset();
		}
		this->fd = fd;
		sent = 0;
		long flags = fcntl(fd, F_GETFL);
		if (flags >= 0) {
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}
		pump.reset(wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE, pump_write, this));
	}

	void stop() {
		pump.reset();
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	}
};

void ClipboardImage::Source::send(wlr_data_source *source, const char *mime_type,
		int32_t fd) {
	auto *src = reinterpret_cast<Source *>(source);
	if (mime_type != nullptr && std::strcmp(mime_type, "image/png") == 0) {
		src->start(fd);
	} else {
		close(fd);
	}
}

int ClipboardImage::Source::pump_write(int fd, uint32_t mask, void *data) {
	auto *src = static_cast<Source *>(data);
	if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
		src->stop();
		return 0;
	}
	while (src->sent < src->bytes.size()) {
		const std::uint8_t *rest = src->bytes.data() + src->sent;
		ssize_t n = write(fd, rest, src->bytes.size() - src->sent);
		if (n > 0) {
			src->sent += static_cast<std::size_t>(n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return 0; // the client will read; the source stays armed
		} else {
			src->stop();
			return 0;
		}
	}
	src->stop();
	return 0;
}

void ClipboardImage::Source::destroy(wlr_data_source *source) {
	auto *src = reinterpret_cast<Source *>(source);
	src->stop();
	// The owner's listener ran first and forgot us; nothing points here now.
	delete src;
}

ClipboardImage::ClipboardImage(Server &server) : server_(server) {
}

ClipboardImage::~ClipboardImage() {
	finish();
}

void ClipboardImage::copy(std::vector<std::uint8_t> png) {
	finish(); // gone() ran; the old source is freed
	auto *source = new Source{};
	source->loop = server_.loop;
	source->bytes = std::move(png);
	static const wlr_data_source_impl impl = {
		.send = &Source::send,
		.accept = nullptr,
		.destroy = &Source::destroy,
	};
	wlr_data_source_init(&source->base, &impl);
	const char *mime = "image/png";
	char **slot = static_cast<char **>(wl_array_add(&source->base.mime_types,
		sizeof(char *)));
	*slot = strdup(mime);
	source->destroyed_.connect<&ClipboardImage::gone>(source->base.events.destroy, this);
	source_ = source;
	wlr_seat_set_selection(server_.seat, &source->base,
		wl_display_next_serial(server_.display));
}

// The seat (or teardown) destroyed our source: forget it before its memory
// goes with the destroy implementation.
void ClipboardImage::gone() {
	if (source_ != nullptr) {
		source_->destroyed_.disconnect(); // the signal asserts no listener remains
		source_ = nullptr;
	}
}

void ClipboardImage::finish() {
	if (source_ != nullptr) {
		wlr_data_source_destroy(&source_->base); // gone() ran; Source freed
	}
}
