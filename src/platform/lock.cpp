#include "platform/lock.h"

#include "core/config.h"
#include "core/server.h"
#include "platform/output.h"

LockSurface::LockSurface(SessionLock &lock, Output &output,
		wlr_session_lock_surface_v1 *wlr)
	: wlr(wlr), output(&output), lock_(lock) {
	map_.connect<&LockSurface::handle_map>(wlr->surface->events.map, this);
	commit_.connect<&LockSurface::handle_commit>(wlr->surface->events.commit, this);
	destroy_.connect<&LockSurface::handle_destroy>(wlr->events.destroy, this);
	configure();
}

LockSurface::~LockSurface() = default;

void LockSurface::configure() {
	wlr_session_lock_surface_v1_configure(wlr,
		static_cast<uint32_t>(output->width), static_cast<uint32_t>(output->height));
}

void LockSurface::handle_map() {
	mapped = true;
	lock_.surface_mapped();
	output->damage();
}

void LockSurface::handle_commit() {
	if (!mapped && wlr->surface->mapped) {
		mapped = true;
		lock_.surface_mapped();
	}
	output->damage();
}

void LockSurface::handle_destroy() {
	lock_.surface_gone(this);
}

SessionLock::SessionLock(Server &server) : server_(server) {}

SessionLock::~SessionLock() = default;

void SessionLock::init() {
	manager_ = wlr_session_lock_manager_v1_create(server_.display);
	if (manager_ == nullptr) {
		wlr_log(WLR_ERROR, "failed to create the session lock manager");
		return;
	}
	new_lock_.connect<&SessionLock::handle_new_lock>(manager_->events.new_lock, this);
	idle_timer_.reset(wl_event_loop_add_timer(server_.loop, handle_idle_timeout, this));
	arm_idle_timer();
}

void SessionLock::finish() {
	idle_timer_.reset();
	new_lock_.disconnect();
	new_surface_.disconnect();
	unlock_.disconnect();
	client_destroy_.disconnect();
	surfaces_.clear();
}

void SessionLock::handle_new_lock(wlr_session_lock_v1 *lock) {
	if (client_ != nullptr) {
		// One locker at a time: the session is already being drawn.
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	client_ = lock;
	locked_sent_ = false;
	new_surface_.connect<&SessionLock::handle_new_surface>(lock->events.new_surface, this);
	unlock_.connect<&SessionLock::handle_unlock>(lock->events.unlock, this);
	client_destroy_.connect<&SessionLock::handle_client_destroy>(lock->events.destroy, this);
	lock_session();
}

void SessionLock::handle_new_surface(wlr_session_lock_surface_v1 *surface) {
	Output *output = surface->output != nullptr
		? static_cast<Output *>(surface->output->data) : nullptr;
	if (output == nullptr) {
		return;
	}
	surfaces_.push_back(std::make_unique<LockSurface>(*this, *output, surface));
}

void SessionLock::handle_unlock() {
	unlock_session();
}

void SessionLock::handle_client_destroy() {
	// A locker that goes without unlocking leaves the session locked: the
	// outputs show an opaque fill until another locker takes over.
	new_surface_.disconnect();
	unlock_.disconnect();
	client_destroy_.disconnect();
	client_ = nullptr;
	locked_sent_ = false;
	surfaces_.clear();
	if (locked_) {
		wlr_log(WLR_INFO, "the locker is gone; the session stays locked");
		arm_idle_timer();
		server_.focus_keyboard(nullptr);
		server_.damage_all();
	}
}

void SessionLock::lock_session() {
	requested_ = false;
	if (!locked_) {
		locked_ = true;
		wlr_log(WLR_INFO, "the session is locked");
	}
	arm_idle_timer();
	server_.input.abandon_interaction();
	server_.launcher.close();
	server_.focus_keyboard(nullptr);
	server_.damage_all();
}

void SessionLock::unlock_session() {
	locked_ = false;
	requested_ = false;
	locked_sent_ = false;
	new_surface_.disconnect();
	unlock_.disconnect();
	client_destroy_.disconnect();
	client_ = nullptr;
	surfaces_.clear();
	wlr_log(WLR_INFO, "the session is unlocked");
	arm_idle_timer();
	server_.refocus_layers();
	if (server_.focused_view != nullptr && server_.focused_layer == nullptr) {
		server_.focus_keyboard(server_.focused_view->surface());
	}
	server_.damage_all();
}

void SessionLock::surface_mapped() {
	refocus();
	send_locked_when_covered();
	server_.damage_all();
}

void SessionLock::surface_gone(LockSurface *surface) {
	std::erase_if(surfaces_, [surface](const std::unique_ptr<LockSurface> &held) {
		return held.get() == surface;
	});
	server_.damage_all();
	refocus();
}

void SessionLock::send_locked_when_covered() {
	if (client_ == nullptr || locked_sent_) {
		return;
	}
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		if (!output->wlr->enabled) {
			continue;
		}
		if (surface_for(*output) == nullptr) {
			return;
		}
	}
	wlr_session_lock_v1_send_locked(client_);
	locked_sent_ = true;
	server_.logind.lock_drawn();
}

wlr_surface *SessionLock::surface_for(const Output &output) const {
	for (const std::unique_ptr<LockSurface> &surface : surfaces_) {
		if (surface->output == &output && surface->mapped) {
			return surface->wlr->surface;
		}
	}
	return nullptr;
}

void SessionLock::refocus() {
	if (!locked_) {
		return;
	}
	Output *under = server_.output_under_cursor();
	if (under != nullptr) {
		if (wlr_surface *surface = surface_for(*under)) {
			server_.focus_keyboard(surface);
			return;
		}
	}
	for (const std::unique_ptr<LockSurface> &surface : surfaces_) {
		if (surface->mapped) {
			server_.focus_keyboard(surface->wlr->surface);
			return;
		}
	}
	server_.focus_keyboard(nullptr);
}

void SessionLock::output_changed(Output &output) {
	for (const std::unique_ptr<LockSurface> &surface : surfaces_) {
		if (surface->output == &output) {
			surface->configure();
		}
	}
	send_locked_when_covered();
}

void SessionLock::notice_activity() {
	arm_idle_timer();
}

void SessionLock::settings_changed() {
	arm_idle_timer();
}

void SessionLock::set_idle_inhibited(bool inhibited) {
	idle_inhibited_ = inhibited;
	arm_idle_timer();
}

// The countdown runs only while the session can be locked and nothing holds
// it off: an idle inhibitor (a video player) keeps the screen awake.
void SessionLock::arm_idle_timer() {
	if (idle_timer_ == nullptr) {
		return;
	}
	int minutes = server_.conf.resolved.lock_idle_minutes;
	idle_armed_ = minutes > 0 && !locked_ && !idle_inhibited_;
	if (!idle_armed_) {
		wl_event_source_timer_update(idle_timer_.get(), 0);
		return;
	}
	wl_event_source_timer_update(idle_timer_.get(), minutes * 60 * 1000);
}

void SessionLock::idle_elapsed() {
	if (!idle_armed_) {
		return; // an inhibitor took the countdown away before it fired
	}
	request("the idle timeout");
}

int SessionLock::handle_idle_timeout(void *data) {
	static_cast<SessionLock *>(data)->idle_elapsed();
	return 0;
}

void SessionLock::request(const char *reason) {
	if (locked_) {
		return;
	}
	requested_ = true;
	wlr_log(WLR_INFO, "lock requested (%s)", reason);
	server_.ipc_events.emit_action(config::lock_action, "{}");
}
