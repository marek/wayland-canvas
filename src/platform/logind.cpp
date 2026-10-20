#include "platform/logind.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include "core/config.h"
#include "core/server.h"

#if CANVAS_HAVE_LOGIND
#include <systemd/sd-bus.h>

namespace {

constexpr const char *login1 = "org.freedesktop.login1";
constexpr const char *manager_path = "/org/freedesktop/login1";
constexpr const char *manager_iface = "org.freedesktop.login1.Manager";
constexpr const char *session_iface = "org.freedesktop.login1.Session";

using BusPtr = Handle<sd_bus, sd_bus_flush_close_unref>;
using MessagePtr = Handle<sd_bus_message, sd_bus_message_unref>;
// Owns what a failed call left in the sd_bus_error it was given.
using ErrorPtr = Handle<sd_bus_error, sd_bus_error_free>;

// What a failed call says: sd-bus' own words when it has any.
const char *why(const sd_bus_error &error, int ret) {
	return error.message != nullptr ? error.message : std::strerror(-ret);
}

} // namespace

// sd-bus gives a pollable fd, so the bus rides the compositor's own event
// loop instead of a poll of its own.
struct Logind::Bus {
	BusPtr bus;
};

Logind::Logind(Server &server) : server_(server) {}

Logind::~Logind() {
	finish();
}

void Logind::init(bool login) {
	if (!login) {
		return; // a nested or headless run leaves the user's session alone
	}
	auto bus = std::make_unique<Bus>();
	sd_bus *raw_bus = nullptr;
	int ret = sd_bus_default_system(&raw_bus);
	bus->bus.reset(raw_bus);
	if (ret < 0 || raw_bus == nullptr) {
		wlr_log(WLR_INFO, "no system bus: no logind lock triggers");
		return;
	}
	// The session this process runs in; its object carries Lock and Unlock.
	sd_bus_error error{}; // SD_BUS_ERROR_NULL is a C99 compound literal
	ErrorPtr error_owner(&error);
	sd_bus_message *raw_reply = nullptr;
	ret = sd_bus_call_method(raw_bus, login1, manager_path, manager_iface,
		"GetSessionByPID", &error, &raw_reply, "u", static_cast<uint32_t>(getpid()));
	MessagePtr reply(raw_reply);
	if (ret < 0) {
		wlr_log(WLR_INFO, "no logind session (%s): no lock triggers from it",
			why(error, ret));
		return;
	}
	const char *session_path = nullptr;
	ret = sd_bus_message_read(reply.get(), "o", &session_path);
	if (ret < 0 || session_path == nullptr) {
		wlr_log(WLR_ERROR, "logind named no session path (%s): no lock triggers",
			std::strerror(-ret));
		return;
	}

	auto on_lock = [](sd_bus_message *, void *data, sd_bus_error *) {
		static_cast<Logind *>(data)->handle_lock();
		return 0;
	};
	auto on_unlock = [](sd_bus_message *, void *data, sd_bus_error *) {
		static_cast<Logind *>(data)->handle_unlock();
		return 0;
	};
	auto on_sleep = [](sd_bus_message *message, void *data, sd_bus_error *) {
		int sleeping = 0;
		if (sd_bus_message_read(message, "b", &sleeping) >= 0) {
			static_cast<Logind *>(data)->handle_prepare_for_sleep(sleeping != 0);
		}
		return 0;
	};
	auto watch = [&](const char *path, const char *iface, const char *member,
			sd_bus_message_handler_t handler) {
		int matched = sd_bus_match_signal(raw_bus, nullptr, login1, path, iface, member,
			handler, this);
		if (matched < 0) {
			wlr_log(WLR_ERROR, "logind: cannot watch %s: %s", member,
				std::strerror(-matched));
		}
		return matched >= 0;
	};
	bool locks = watch(session_path, session_iface, "Lock", on_lock);
	watch(session_path, session_iface, "Unlock", on_unlock);
	bool sleeps = watch(manager_path, manager_iface, "PrepareForSleep", on_sleep);
	if (!locks && !sleeps) {
		wlr_log(WLR_ERROR, "logind: nothing to listen for: no lock triggers");
		return;
	}

	int fd = sd_bus_get_fd(raw_bus);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "the system bus has no pollable fd (%s): no lock triggers",
			std::strerror(-fd));
		return;
	}
	source_.reset(wl_event_loop_add_fd(server_.loop, fd, WL_EVENT_READABLE,
		handle_bus, this));
	if (source_ == nullptr) {
		wlr_log(WLR_ERROR, "the system bus could not join the event loop: no lock triggers");
		return;
	}
	bus_ = std::move(bus);
	wait_timer_.reset(wl_event_loop_add_timer(server_.loop, handle_wait_timeout, this));
	if (wait_timer_ == nullptr) {
		wlr_log(WLR_ERROR, "logind: no timer for the sleep wait: the lock may draw late");
	}
	// A delay is held only for a sleep we are told of: unheard, it would
	// make every sleep wait out logind's delay limit.
	if (sleeps) {
		take_sleep_inhibitor();
	}
}

void Logind::finish() {
	release_sleep_inhibitor();
	wait_timer_.reset();
	source_.reset();
	bus_.reset();
}

int Logind::handle_bus(int, uint32_t, void *data) {
	auto *self = static_cast<Logind *>(data);
	if (self->bus_ == nullptr) {
		return 0;
	}
	sd_bus *bus = self->bus_->bus.get();
	int ret = 0;
	while ((ret = sd_bus_process(bus, nullptr)) > 0) {
	}
	if (ret >= 0) {
		ret = sd_bus_flush(bus);
	}
	if (ret < 0) {
		// A bus that failed stays readable: left in the loop, it would spin.
		wlr_log(WLR_ERROR, "the system bus failed (%s): no more lock triggers",
			std::strerror(-ret));
		self->finish();
	}
	return 0;
}

void Logind::handle_lock() {
	server_.lock.request("loginctl lock-session");
}

void Logind::handle_unlock() {
	// Unlocking is the locker's own decision: it asked for a password.
	wlr_log(WLR_INFO, "logind asked to unlock; the locker decides");
}

void Logind::handle_prepare_for_sleep(bool sleeping) {
	sleeping_ = sleeping;
	if (!sleeping) {
		// Awake again: hold the delay for the next sleep.
		take_sleep_inhibitor();
		return;
	}
	server_.lock.request("the machine is going to sleep");
	if (server_.lock.locked()) {
		release_sleep_inhibitor();
		return;
	}
	// The lock is drawn before the machine sleeps; a locker that never
	// draws must not hold the machine awake forever.
	if (wait_timer_ != nullptr) {
		wl_event_source_timer_update(wait_timer_.get(), config::lock_sleep_wait_ms);
	}
}

int Logind::handle_wait_timeout(void *data) {
	auto *self = static_cast<Logind *>(data);
	wlr_log(WLR_ERROR, "no lock was drawn before the sleep: letting it go ahead");
	self->release_sleep_inhibitor();
	return 0;
}

void Logind::lock_drawn() {
	if (sleeping_) {
		release_sleep_inhibitor();
	}
}

void Logind::take_sleep_inhibitor() {
	if (bus_ == nullptr || inhibitor_fd_ >= 0) {
		return;
	}
	sd_bus_error error{}; // SD_BUS_ERROR_NULL is a C99 compound literal
	ErrorPtr error_owner(&error);
	sd_bus_message *raw_reply = nullptr;
	int ret = sd_bus_call_method(bus_->bus.get(), login1, manager_path, manager_iface,
		"Inhibit", &error, &raw_reply, "ssss", "sleep", "wayland-canvas",
		"Locking the screen before sleep", "delay");
	MessagePtr reply(raw_reply);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "logind refused a sleep delay (%s): the lock may draw late",
			why(error, ret));
		return;
	}
	int fd = -1;
	ret = sd_bus_message_read(reply.get(), "h", &fd);
	if (ret < 0 || fd < 0) {
		wlr_log(WLR_ERROR, "logind gave no sleep delay to hold (%s)", std::strerror(-ret));
		return;
	}
	inhibitor_fd_ = dup(fd); // the message owns the one it handed over
	if (inhibitor_fd_ < 0) {
		wlr_log(WLR_ERROR, "cannot keep the sleep delay: %s", std::strerror(errno));
	}
}

void Logind::release_sleep_inhibitor() {
	if (wait_timer_ != nullptr) {
		wl_event_source_timer_update(wait_timer_.get(), 0);
	}
	if (inhibitor_fd_ >= 0) {
		close(inhibitor_fd_);
		inhibitor_fd_ = -1;
	}
}

#else // CANVAS_HAVE_LOGIND

Logind::Logind(Server &) {}
Logind::~Logind() = default;

void Logind::init(bool) {
	wlr_log(WLR_INFO, "built without logind: no lock on lock-session or sleep");
}

void Logind::finish() {}
void Logind::lock_drawn() {}
void Logind::handle_lock() {}
void Logind::handle_unlock() {}
void Logind::handle_prepare_for_sleep(bool) {}
void Logind::take_sleep_inhibitor() {}
void Logind::release_sleep_inhibitor() {}
int Logind::handle_bus(int, uint32_t, void *) { return 0; }
int Logind::handle_wait_timeout(void *) { return 0; }

#endif
