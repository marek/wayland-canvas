#pragma once

#include "core/util.h"

class Server;

// The seat manager's half of the lock: `loginctl lock-session` and the
// sleep the machine is about to take. Only a login session watches it — a
// nested or headless run never touches the logind session it was started
// from.
class Logind {
public:
	explicit Logind(Server &server);
	~Logind();
	Logind(const Logind &) = delete;
	Logind &operator=(const Logind &) = delete;

	void init(bool login);
	void finish();

	// The locker has drawn every output: the machine may sleep now.
	void lock_drawn();

private:
	struct Bus; // the system-bus connection (logind.cpp)

	void handle_lock();
	void handle_unlock();
	void handle_prepare_for_sleep(bool sleeping);
	// A delay inhibitor, so the lock is on the screen before the machine
	// sleeps; released by lock_drawn() or when the wait runs out.
	void take_sleep_inhibitor();
	void release_sleep_inhibitor();
	static int handle_bus(int fd, uint32_t mask, void *data);
	static int handle_wait_timeout(void *data);

	Server &server_;
	std::unique_ptr<Bus> bus_;
	EventSourcePtr source_;
	EventSourcePtr wait_timer_;
	int inhibitor_fd_ = -1;
	bool sleeping_ = false;
};
