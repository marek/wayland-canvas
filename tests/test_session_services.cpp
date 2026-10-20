// The rules for the session's own services: who manages them, and what a
// login runs to start and stop them. Pure strings and decisions — nothing
// here speaks to a systemd manager.
#include <string>
#include "check.h"
#include "platform/session_services.h"

namespace {

using unit::check;

} // namespace

int main() {
	using session_bus::Owner;
	check(session_services::manages(Owner::login),
		"a login on the user's bus owns the session services");
	check(!session_services::manages(Owner::host),
		"a nested or headless run on the user's bus owns none of them");
	check(!session_services::manages(Owner::own),
		"a run on a bus of its own starts none");
	check(!session_services::manages(Owner::none),
		"a run with no session bus starts none");

	std::string start = session_services::start_command();
	std::string stop = session_services::stop_command();
	check(start.find(session_services::target) != std::string::npos,
		"the start command names the session target");
	check(start.find("stop") < start.find("start"),
		"it stops what a crashed session left before starting its own");
	check(start.find("--user") != std::string::npos,
		"it works on the user's own manager, never the system one");
	check(stop == std::string("systemctl --user stop ")
			+ session_services::target,
		"the stop command stops exactly that target");
	return unit::summary();
}
