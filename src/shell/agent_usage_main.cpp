// wayland-canvas-agent-usage: the helper the agents panel's widget runs
// (docs/intents/shell/bar.md). The library owns every counting rule; this
// file only resolves the environment, probes the two limits services and
// prints the one JSON record the panel reads. The Anthropic probe goes
// through curl with the CLI's own saved credentials — nothing else ever
// reads them — and the Codex probe speaks the app-server's JSON-RPC the
// way the codex CLI itself does.
#include "shell/agent_usage.h"

#include <json-glib/json-glib.h>

#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "core/fs.h"
#include "core/json.h"

namespace {

using namespace agent_usage;

constexpr const char *claude_auth_help =
	"Run `claude auth login` to restore authoritative usage.";
constexpr const char *codex_auth_help =
	"Run `codex login` to authenticate.";
// A panel opened and shut repeatedly must not turn into a probe per
// flick; a fresh answer is reused for this long.
constexpr int probe_reuse_seconds = 15;

std::string home_path(const char *variable, const char *relative) {
	const char *value = variable != nullptr ? std::getenv(variable) : nullptr;
	if (value != nullptr && *value != '\0') {
		return value;
	}
	const char *home = std::getenv("HOME");
	if (home == nullptr || *home == '\0') {
		return relative + 1; // no HOME: a relative best-effort path
	}
	return std::string{home} + "/" + relative;
}

// A forked child: its stdout comes back on one pipe, its stdin is fed
// through another when asked, and its death is tied to this process, so a
// killed run leaks nothing.
struct Spawned {
	pid_t pid = -1;
	int out = -1; // read the child's stdout here
	int in = -1;  // write the child's stdin here, -1 when not asked for
};

Spawned spawn(const char *const argv[], bool with_stdin = false) {
	int out_fds[2];
	int in_fds[2];
	if (pipe(out_fds) != 0 || (with_stdin && pipe(in_fds) != 0)) {
		return {};
	}
	pid_t pid = fork();
	if (pid == 0) {
		::close(out_fds[0]);
		dup2(out_fds[1], STDOUT_FILENO);
		::close(out_fds[1]);
		if (with_stdin) {
			::close(in_fds[1]);
			dup2(in_fds[0], STDIN_FILENO);
			::close(in_fds[0]);
		}
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		execvp(argv[0], const_cast<char *const *>(argv));
		_exit(127);
	}
	::close(out_fds[1]);
	if (with_stdin) {
		::close(in_fds[0]);
	}
	return pid > 0 ? Spawned{pid, out_fds[0], with_stdin ? in_fds[1] : -1}
		       : Spawned{};
}

// The child's whole stdout within `timeout_s`; the child is reaped either
// way. Closing `stdin_fd` first lets a well-behaved child finish on EOF.
std::string child_output(Spawned &child, int timeout_s, int stdin_fd = -1) {
	if (stdin_fd >= 0) {
		::close(stdin_fd);
	}
	std::string out;
	char buf[4096];
	std::time_t deadline = std::time(nullptr) + timeout_s;
	while (std::time(nullptr) < deadline) {
		pollfd p = {.fd = child.out, .events = POLLIN, .revents = 0};
		if (poll(&p, 1, 200) <= 0 || (p.revents & (POLLIN | POLLHUP)) == 0) {
			continue;
		}
		ssize_t n = read(child.out, buf, sizeof buf);
		if (n <= 0) {
			break;
		}
		out.append(buf, static_cast<std::size_t>(n));
	}
	kill(child.pid, SIGKILL); // a no-op on an already-exited child
	waitpid(child.pid, nullptr, 0);
	::close(child.out);
	child.out = -1;
	return out;
}

void reap(Spawned &child) {
	if (child.pid <= 0) {
		return;
	}
	if (child.in >= 0) {
		::close(child.in);
		child.in = -1;
	}
	kill(child.pid, SIGKILL);
	waitpid(child.pid, nullptr, 0);
	if (child.out >= 0) {
		::close(child.out);
		child.out = -1;
	}
}

// ---------------------------------------------------------- Claude limits

struct Credentials {
	std::string token;
	long long expires_at_ms = 0;
	std::string tier;
};

// The CLI's own saved sign-in: the access token, its expiry and the plan
// labels. Nothing but the probe's Authorization header ever carries any
// of it, and only the plan label reaches the printed record.
Credentials claude_credentials(const std::string &claude_dir) {
	Credentials out;
	JsonParserPtr parser = fs::read_json_file(claude_dir + "/.credentials.json");
	JsonObject *root = root_object(parser);
	if (root == nullptr) {
		return out;
	}
	JsonObject *login = json_object_has_member(root, "claudeAiOauth")
		? json_object_get_object_member(root, "claudeAiOauth")
		: nullptr;
	if (login == nullptr) {
		return out;
	}
	auto text = [login](const char *name) -> std::string {
		if (!json_object_has_member(login, name)) {
			return {};
		}
		const char *value = json_object_get_string_member(login, name);
		return value != nullptr ? std::string{value} : std::string{};
	};
	auto number = [login](const char *name) -> long long {
		if (!json_object_has_member(login, name)) {
			return 0;
		}
		JsonNode *node = json_object_get_member(login, name);
		double value = json_node_get_double(node);
		return std::isfinite(value) ? std::llround(value) : 0;
	};
	out.token = text("accessToken");
	out.expires_at_ms = number("expiresAt");
	out.tier = plan_label(text("rateLimitTier"), text("subscriptionType"));
	return out;
}

// GET with the bearer token, through curl; empty on any failure.
std::string curl_bearer(const std::string &url, const std::string &token) {
	std::string authorization = "Authorization: Bearer " + token;
	const char *const argv[] = {"curl", "-sS", "--max-time", "10",
		"-H", authorization.c_str(),
		"-H", "anthropic-beta: oauth-2025-04-20",
		"-H", "Accept: application/json",
		url.c_str(), static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return {};
	}
	return child_output(child, 15);
}

ProviderLimits claude_limits(const std::string &claude_dir,
	const std::string &cache_dir) {
	Credentials credentials = claude_credentials(claude_dir);
	ProviderLimits out;
	out.tier = credentials.tier;
	out.help = claude_auth_help;
	// Only the CLI can mint a fresh token: it refreshes the credential
	// file when it runs, so a machine left alone long enough finds the
	// saved token lapsed. Say so, and keep the local figures.
	if (credentials.token.empty()) {
		out.status = "Waiting for auth";
		return out;
	}
	if (credentials.expires_at_ms > 0
			&& credentials.expires_at_ms <= double(std::time(nullptr)) * 1000) {
		out.status = "Sign-in expired";
		return out;
	}
	std::string cache_file = cache_dir + "/agent-usage-claude-limits.json";
	std::optional<LimitsCache> cached = read_limits_cache(cache_file);
	if (cached.has_value() && !cached->limits.empty()
			&& std::time(nullptr) - cached->fetched_at_ms / 1000
				< probe_reuse_seconds) {
		out.limits = cached->limits;
		return out;
	}
	const char *override = std::getenv("WAYLAND_CANVAS_CLAUDE_USAGE_URL");
	std::string payload = curl_bearer(
		override != nullptr && *override != '\0' ? override
			: "https://api.anthropic.com/api/oauth/usage",
		credentials.token);
	std::vector<Limit> limits = parse_anthropic_limits(payload);
	if (!limits.empty()) {
		out.limits = limits;
		write_limits_cache(cache_file,
			LimitsCache{
				.fetched_at_ms = std::llround(
					double(std::time(nullptr)) * 1000),
				.limits = limits,
			});
		return out;
	}
	// No answer worth showing: the last numbers whose window has not reset
	// stay on the panel, and the state line says what happened.
	std::vector<Limit> fallback = open_limits(
		cached.has_value() ? cached->limits : std::vector<Limit>{});
	if (!fallback.empty()) {
		out.limits = fallback;
		return out;
	}
	out.status = "Limits unavailable";
	out.help = "Couldn't reach Anthropic's usage endpoint. "
		"Local stats are still shown.";
	return out;
}

// ------------------------------------------------------------- Codex RPC

bool write_all(int fd, std::string_view text) {
	std::size_t sent = 0;
	while (sent < text.size()) {
		ssize_t n = write(fd, text.data() + sent, text.size() - sent);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<std::size_t>(n);
	}
	return true;
}

// One JSON-RPC line answered with this id within the timeout, or "". The
// app-server may put notifications on the wire first; only the line
// carrying the id asked for is the answer.
std::string rpc_line(int fd, int id, int timeout_s) {
	std::string buffer;
	char chunk[4096];
	std::time_t deadline = std::time(nullptr) + timeout_s;
	while (std::time(nullptr) < deadline) {
		std::size_t newline = buffer.find('\n');
		if (newline != std::string::npos) {
			std::string line = buffer.substr(0, newline);
			buffer.erase(0, newline + 1);
			if (line.find(std::format(R"("id":{})", id))
					!= std::string::npos
				|| line.find(std::format(R"("id": {})", id))
					!= std::string::npos) {
				return line;
			}
			continue;
		}
		pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&p, 1, 200) <= 0 || (p.revents & (POLLIN | POLLHUP)) == 0) {
			continue;
		}
		ssize_t n = read(fd, chunk, sizeof chunk);
		if (n <= 0) {
			return {};
		}
		buffer.append(chunk, static_cast<std::size_t>(n));
	}
	return {};
}

// A response line that carries an error member: the app-server's refusal,
// which for these reads is the not-signed-in answer.
bool rpc_is_error(const std::string &line) {
	JsonParserPtr parser = parse_json(line);
	JsonObject *root = root_object(parser);
	return root != nullptr && json_object_has_member(root, "error");
}

ProviderLimits codex_limits() {
	ProviderLimits out;
	out.help = codex_auth_help;
	const char *const which[] = {"/bin/sh", "-c", "command -v codex",
		static_cast<const char *>(nullptr)};
	Spawned finder = spawn(which);
	std::string path = child_output(finder, 5);
	while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
		path.pop_back();
	}
	if (path.empty()) {
		out.status = "Codex unavailable";
		out.help = "The codex CLI was not found on PATH.";
		return out;
	}
	const char *const argv[] = {path.c_str(), "-s", "read-only",
		"-a", "on-request", "app-server", static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv, true);
	if (child.pid <= 0) {
		out.status = "Codex unavailable";
		out.help = "The codex CLI was not found on PATH.";
		return out;
	}
	std::string account;
	std::string rate_limits;
	bool transport_ok = write_all(child.in,
			R"({"id":1,"method":"initialize","params":{"clientInfo":)"
			R"({"name":"wayland-canvas-agent-usage","version":"1"}}})"
			"\n")
		&& !rpc_line(child.out, 1, 8).empty()
		&& write_all(child.in, R"({"method":"initialized","params":{}})"
			"\n");
	if (transport_ok) {
		write_all(child.in,
			R"({"id":2,"method":"account/read","params":{}})"
			"\n");
		account = rpc_line(child.out, 2, 4);
		if (!account.empty()) {
			write_all(child.in,
				R"({"id":3,"method":"account/rateLimits/read","params":{}})"
				"\n");
			rate_limits = rpc_line(child.out, 3, 4);
		}
	}
	bool auth_error = rpc_is_error(rate_limits);
	reap(child);
	if (!transport_ok || account.empty() || rate_limits.empty()) {
		out.status = "Codex limits unavailable";
		out.help = "Couldn't reach the Codex app-server. "
			"Local stats are still shown.";
		return out;
	}
	if (auth_error) {
		out.status = "Waiting for auth";
		return out;
	}
	CodexLimits parsed = parse_codex_limits(account, rate_limits);
	out.limits = parsed.limits;
	out.tier = parsed.plan;
	return out;
}

} // namespace

int main() {
	Paths paths;
	paths.claude_dir = home_path("CLAUDE_CONFIG_DIR", ".claude");
	paths.codex_home = home_path("CODEX_HOME", ".codex");
	const char *home = std::getenv("HOME");
	std::string home_dir = home != nullptr && *home != '\0' ? home : ".";
	paths.pi_sessions = home_dir + "/.pi/agent/sessions";
	paths.omp_sessions = home_dir + "/.omp/agent/sessions";
	paths.opencode_db = home_path("XDG_DATA_HOME", ".local/share")
		+ "/opencode/opencode.db";
	std::string cache_dir = home_path("XDG_CACHE_HOME", ".cache")
		+ "/wayland-canvas";
	paths.cache_file = cache_dir + "/agent-usage.json";

	std::map<std::string, ProviderLimits> limits;
	limits["claude"] = claude_limits(paths.claude_dir, cache_dir);
	limits["gpt"] = codex_limits();

	Record record = collect(paths, limits);
	std::string json = record_json(record);
	std::fwrite(json.data(), 1, json.size(), stdout);
	std::putc('\n', stdout);
	return 0;
}
