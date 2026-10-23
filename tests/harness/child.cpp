#include "harness/child.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "harness/files.h"

namespace harness {

namespace {

constexpr int stop_grace_ms = 2000;
constexpr int stop_poll_us = 10'000;

// Never destroyed: a Child that lives in a static is stopped after the
// statics declared later are gone.
std::vector<Child *> &started() {
	static auto *children = new std::vector<Child *>;
	return *children;
}

// True when `pid` is gone (reaped now, or by someone else).
bool reaped(pid_t pid) {
	pid_t got = waitpid(pid, nullptr, WNOHANG);
	return got == pid || got < 0;
}

} // namespace

Child::~Child() {
	stop();
}

bool Child::start(const std::vector<std::string> &argv) {
	stop();
	if (argv.empty()) {
		return false;
	}
	std::vector<char *> args;
	for (const std::string &arg : argv) {
		args.push_back(const_cast<char *>(arg.c_str()));
	}
	args.push_back(nullptr);
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	pid_ = fork();
	if (pid_ == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, nullptr);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		execvp(args[0], args.data());
		_exit(127);
	}
	close(fds[1]);
	if (pid_ < 0) {
		close(fds[0]);
		return false;
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	out_ = fds[0];
	said_.clear();
	started().push_back(this);
	return true;
}

bool Child::start_client(const char *env_name, const std::vector<std::string> &args) {
	const char *bin = std::getenv(env_name);
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	std::vector<std::string> argv{bin};
	argv.insert(argv.end(), args.begin(), args.end());
	return start(argv);
}

bool Child::running() {
	if (pid_ <= 0) {
		return false;
	}
	if (!reaped(pid_)) {
		return true;
	}
	pid_ = -1;
	return false;
}

void Child::drain() {
	char buf[512];
	ssize_t got = 0;
	while (out_ >= 0 && (got = read(out_, buf, sizeof buf)) > 0) {
		said_.append(buf, static_cast<std::size_t>(got));
	}
}

std::string_view Child::said() {
	drain();
	return said_;
}

int Child::count(std::string_view word) {
	return count_of(said(), word);
}

bool Child::said_line(std::string_view line, std::size_t from) {
	std::string_view all = said();
	for (std::size_t at = all.find(line, from); at != std::string_view::npos;
			at = all.find(line, at + 1)) {
		bool starts = at == 0 || all[at - 1] == '\n';
		bool ends = at + line.size() == all.size() || all[at + line.size()] == '\n';
		if (starts && ends) {
			return true;
		}
	}
	return false;
}

std::string Child::since(std::size_t from) {
	std::string_view all = said();
	std::string out{from < all.size() ? all.substr(from) : std::string_view{}};
	std::ranges::replace(out, '\n', '|');
	return out;
}

bool Child::prints(std::string_view marker, int timeout_ms) {
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (said().find(marker) == std::string_view::npos) {
		if (out_ < 0 || std::chrono::steady_clock::now() >= until || !running()) {
			return said().find(marker) != std::string_view::npos;
		}
		pollfd waiting{.fd = out_, .events = POLLIN, .revents = 0};
		poll(&waiting, 1, 50);
	}
	return true;
}

void Child::signal(int number) {
	if (pid_ > 0) {
		::kill(pid_, number);
	}
}

void Child::close_pipe() {
	if (out_ >= 0) {
		close(out_);
		out_ = -1;
	}
}

void Child::kill() {
	if (pid_ > 0) {
		::kill(pid_, SIGKILL);
		waitpid(pid_, nullptr, 0);
		pid_ = -1;
	}
	close_pipe();
	std::erase(started(), this);
}

void Child::stop() {
	if (pid_ > 0) {
		::kill(pid_, SIGTERM);
		for (int waited = 0; pid_ > 0 && waited < stop_grace_ms * 1000; waited += stop_poll_us) {
			if (reaped(pid_)) {
				pid_ = -1;
				break;
			}
			usleep(stop_poll_us);
		}
	}
	kill();
}

void stop_children() {
	std::vector<Child *> remaining = started();
	for (Child *child : remaining) {
		child->stop();
	}
}

} // namespace harness
