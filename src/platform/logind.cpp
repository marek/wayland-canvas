#include "platform/logind.h"

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

} // namespace

// sd-bus gives a pollable fd, so the bus rides the compositor's own event
// loop instead of a poll of its own.
struct Logind::Bus {
	sd_bus *bus = nullptr;
	std::string session_path;

	~Bus() {
		if (bus != nullptr) {
			sd_bus_flush_close_unref(bus);
		}
	}
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
	if (sd_bus_default_system(&bus->bus) < 0 || bus->bus == nullptr) {
		wlr_log(WLR_INFO, "no system bus: no logind lock triggers");
		return;
	}
	// The session this process runs in; its object carries Lock and Unlock.
	sd_bus_error error{}; // SD_BUS_ERROR_NULL is a C99 compound literal
	sd_bus_message *reply = nullptr;
	const char *path = nullptr;
	int ret = sd_bus_call_method(bus->bus, login1, manager_path, manager_iface,
		"GetSessionByPID", &error, &reply, "u", static_cast<uint32_t>(getpid()));
	if (ret >= 0 && sd_bus_message_read(reply, "o", &path) >= 0 && path != nullptr) {
		bus->session_path = path;
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	if (bus->session_path.empty()) {
		wlr_log(WLR_INFO, "no logind session: no lock triggers from it");
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
	sd_bus_match_signal(bus->bus, nullptr, login1, bus->session_path.c_str(),
		session_iface, "Lock", on_lock, this);
	sd_bus_match_signal(bus->bus, nullptr, login1, bus->session_path.c_str(),
		session_iface, "Unlock", on_unlock, this);
	sd_bus_match_signal(bus->bus, nullptr, login1, manager_path, manager_iface,
		"PrepareForSleep", on_sleep, this);

	int fd = sd_bus_get_fd(bus->bus);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "the system bus has no pollable fd: no lock triggers");
		return;
	}
	bus_ = std::move(bus);
	source_.reset(wl_event_loop_add_fd(server_.loop, fd, WL_EVENT_READABLE,
		handle_bus, this));
	wait_timer_.reset(wl_event_loop_add_timer(server_.loop, handle_wait_timeout, this));
	take_sleep_inhibitor();
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
	while (sd_bus_process(self->bus_->bus, nullptr) > 0) {
	}
	sd_bus_flush(self->bus_->bus);
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
	sd_bus_message *reply = nullptr;
	int ret = sd_bus_call_method(bus_->bus, login1, manager_path, manager_iface,
		"Inhibit", &error, &reply, "ssss", "sleep", "wayland-canvas",
		"Locking the screen before sleep", "delay");
	int fd = -1;
	if (ret >= 0 && sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0) {
		inhibitor_fd_ = dup(fd); // the message owns the one it handed over
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
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

struct Logind::Bus {};

Logind::Logind(Server &server) : server_(server) {}
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
