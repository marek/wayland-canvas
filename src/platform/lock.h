#pragma once

#include <memory>
#include <string>
#include <vector>
#include "core/util.h"
#include "input/input_gate.h"

class Output;
class Server;
class SessionLock;

// One locker surface: the whole of one output while the session is locked.
class LockSurface {
public:
	LockSurface(SessionLock &lock, Output &output, wlr_session_lock_surface_v1 *wlr);
	~LockSurface();
	LockSurface(const LockSurface &) = delete;
	LockSurface &operator=(const LockSurface &) = delete;

	// Re-sends the output's size; the locker answers with a commit.
	void configure();

	wlr_session_lock_surface_v1 *wlr = nullptr;
	Output *output = nullptr;
	bool mapped = false;

private:
	void handle_map();
	void handle_commit();
	void handle_destroy();
	SessionLock &lock_;
	Listener map_, commit_, destroy_;
};

// ext-session-lock-v1. While the session is locked only the locker's surfaces
// are drawn and receive input, and a capture shows what the screen shows: the
// locker, or an opaque fill where it has no surface yet. A locker that dies
// leaves the session locked; the next locker takes it over.
class SessionLock final : public InputGate {
public:
	explicit SessionLock(Server &server);
	~SessionLock();
	SessionLock(const SessionLock &) = delete;
	SessionLock &operator=(const SessionLock &) = delete;

	void init();
	void finish();

	bool locked() const override { return locked_; }
	// True while the session is locked with no client to draw it: a locker
	// that died, or one that has not drawn this output yet.
	bool abandoned() const { return locked_ && client_ == nullptr; }
	// The locker's mapped surface for `output`, nullptr when it has none.
	wlr_surface *surface_for(const Output &output) const override;
	// Gives the keyboard to the locker's surface under the cursor, else to
	// any of its surfaces; clears the focus when it has none.
	void refocus();
	// A new output while locked needs a surface of its own, and an output
	// that resized a new configure.
	void output_changed(Output &output);

	// A lock trigger: the `lock` action, a logind Lock, the idle timeout. The
	// shell's locker draws the lock; the session is locked once it takes the
	// protocol, and `requested()` holds until then.
	void request(const char *reason);
	// Whether a lock was asked for and no locker has taken it yet.
	bool requested() const { return requested_; }

	// Input of any kind: the idle countdown starts again.
	void notice_activity() override;
	// The countdown ran out: locks unless something holds it off.
	void idle_elapsed();
	// Whether the idle countdown is running at all.
	bool idle_armed() const { return idle_armed_; }
	// The idle timeout in canvas.conf changed.
	void settings_changed();
	// An idle inhibitor came or went: the countdown stands down while one
	// is alive (a video player holding the screen awake).
	void set_idle_inhibited(bool inhibited);

	// LockSurface's, called as its surfaces map and go.
	void surface_mapped();
	void surface_gone(LockSurface *surface);

private:
	void handle_new_lock(wlr_session_lock_v1 *lock);
	void handle_new_surface(wlr_session_lock_surface_v1 *surface);
	void handle_unlock();
	void handle_client_destroy();
	// Sends `locked` once every enabled output shows a locker surface.
	void send_locked_when_covered();
	void lock_session();
	void unlock_session();

	// Restarts (or stops) the idle countdown from the settings in force.
	void arm_idle_timer();
	static int handle_idle_timeout(void *data);

	Server &server_;
	EventSourcePtr idle_timer_;
	wlr_session_lock_manager_v1 *manager_ = nullptr;
	wlr_session_lock_v1 *client_ = nullptr; // the live locker, if any
	std::vector<std::unique_ptr<LockSurface>> surfaces_;
	bool locked_ = false;
	bool locked_sent_ = false;
	bool requested_ = false;
	bool idle_armed_ = false;
	bool idle_inhibited_ = false;
	Listener new_lock_, new_surface_, unlock_, client_destroy_;
};
