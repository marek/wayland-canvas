#include "core/process.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/wlr.h"

namespace {

// SIGPIPE as the compositor found it.
void (*inherited_sigpipe)(int) = SIG_DFL;
bool sigpipe_recorded = false;

} // namespace

double now_ms() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

pid_t spawn(const char *cmd) {
	// The intermediate child reports the command's pid through a pipe, so a
	// launch can be traced to the windows its processes open.
	int report[2];
	if (pipe2(report, O_CLOEXEC) < 0) {
		wlr_log_errno(WLR_ERROR, "pipe failed");
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		close(report[0]);
		close(report[1]);
		return -1;
	}
	if (pid == 0) {
		// Double fork so the command is reparented and never becomes a zombie.
		close(report[0]);
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		std::signal(SIGPIPE, inherited_sigpipe);
		pid_t command = fork();
		if (command == 0) {
			// The command starts with only its three standard fds:
			// nothing the compositor holds open reaches it.
			close_range(STDERR_FILENO + 1, ~0U, 0);
			execl("/bin/sh", "/bin/sh", "-c", cmd, static_cast<char *>(nullptr));
			_exit(127);
		}
		ssize_t sent = write(report[1], &command, sizeof command);
		_exit(sent == static_cast<ssize_t>(sizeof command) ? 0 : 1);
	}
	close(report[1]);
	pid_t command = -1;
	ssize_t got = 0;
	do {
		got = read(report[0], &command, sizeof command);
	} while (got < 0 && errno == EINTR);
	close(report[0]);
	waitpid(pid, nullptr, 0);
	return got == static_cast<ssize_t>(sizeof command) ? command : -1;
}

void ignore_sigpipe() {
	void (*previous)(int) = std::signal(SIGPIPE, SIG_IGN);
	if (sigpipe_recorded) {
		return;
	}
	// A handler would not survive exec: only an ignore is passed on.
	inherited_sigpipe = previous == SIG_IGN ? SIG_IGN : SIG_DFL;
	sigpipe_recorded = true;
}
