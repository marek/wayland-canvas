#include "harness/control.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "core/server.h"
#include "harness/check.h"
#include "harness/files.h"
#include "platform/ipc_client.h"

namespace harness {

namespace {

constexpr int poll_ms = 400;
constexpr int give_up_ms = 10'000;
constexpr int quit_budget_ms = 5000;

std::vector<pid_t> forked;

// The forked client's whole life; never returns.
[[noreturn]] void talk(std::string_view to_send, const std::string &out_path,
		ControlClient options) {
	int fd = connect_to_socket();
	FILE *out = std::fopen(out_path.c_str(), "w");
	if (fd < 0 || out == nullptr) {
		if (out != nullptr) {
			std::fclose(out);
		}
		_exit(125);
	}
	if (!write_request(fd, to_send)) {
		std::fclose(out);
		close(fd);
		_exit(124);
	}
	if (options.half_close) {
		shutdown(fd, SHUT_WR);
	}
	char buf[1024];
	int idle = 0;
	bool eof = false;
	// No cap on what is read: a large answer must be drained whole; the only
	// bound is the time.
	auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(give_up_ms);
	while (!eof && idle < options.quiet_polls && std::chrono::steady_clock::now() < give_up) {
		pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&waiting, 1, poll_ms) == 0) {
			idle++;
			continue;
		}
		idle = 0;
		if ((waiting.revents & (POLLERR | POLLNVAL)) != 0) {
			break;
		}
		ssize_t got = read(fd, buf, sizeof buf);
		if (got <= 0) {
			eof = true;
			break;
		}
		std::fwrite(buf, 1, static_cast<std::size_t>(got), out);
		std::fflush(out); // the scenario reads the file while this child lives
	}
	if (eof) {
		std::fputs("\nEOF\n", out);
	}
	std::fclose(out);
	close(fd);
	_exit(0);
}

} // namespace

pid_t fork_client(const std::function<void()> &body) {
	pid_t pid = fork();
	if (pid == 0) {
		body();
		_exit(0);
	}
	if (pid > 0) {
		forked.push_back(pid);
	}
	return pid;
}

pid_t fork_control_client(std::string_view to_send, std::string_view out,
		ControlClient options) {
	std::string out_path = std::format("{}/{}", scratch(), out);
	return fork_client([&] { talk(to_send, out_path, options); });
}

std::string control_answer(std::string_view out) {
	return read_text(std::format("{}/{}", scratch(), out));
}

bool control_client_done(pid_t pid) {
	if (pid <= 0) {
		return true;
	}
	pid_t got = waitpid(pid, nullptr, WNOHANG);
	if (got == 0) {
		return false;
	}
	std::erase(forked, pid);
	return true;
}

void stop_control_clients() {
	for (pid_t pid : forked) {
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	}
	forked.clear();
}

void run_command(const std::string &shell, std::string_view name) {
	std::error_code stale;
	std::filesystem::remove(std::format("{}/{}.code", scratch(), name), stale);
	spawn(std::format("{} > {}/{}.out 2> {}/{}.err; echo $? > {}/{}.code", shell, scratch(),
		name, scratch(), name, scratch(), name).c_str());
}

void run_msg(std::string_view method, std::string_view params, std::string_view name) {
	std::string shell = std::format("\"{}\" msg {}", std::getenv("WAYLAND_CANVAS_BIN"), method);
	if (!params.empty()) {
		shell += std::format(" '{}'", params);
	}
	run_command(shell, name);
}

bool command_done(std::string_view name) {
	return file_exists(std::format("{}/{}.code", scratch(), name));
}

namespace {

std::string trimmed_file(std::string_view name, std::string_view extension) {
	std::string text = read_text(std::format("{}/{}.{}", scratch(), name, extension));
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
		text.pop_back();
	}
	return text;
}

} // namespace

std::string command_out(std::string_view name) {
	return trimmed_file(name, "out");
}

std::string command_err(std::string_view name) {
	return trimmed_file(name, "err");
}

int command_code(std::string_view name) {
	return std::atoi(trimmed_file(name, "code").c_str());
}

JsonAnswer::JsonAnswer(std::string_view text) : parser_(json_parser_new()) {
	parsed_ = json_parser_load_from_data(parser_, text.data(), static_cast<gssize>(text.size()),
		nullptr);
}

JsonAnswer::~JsonAnswer() {
	g_object_unref(parser_);
}

JsonObject *JsonAnswer::object() const {
	JsonNode *root = parsed_ ? json_parser_get_root(parser_) : nullptr;
	return root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
}

std::optional<long long> member_int(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return std::nullopt;
	}
	JsonNode *node = json_object_get_member(object, name);
	return json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_INT64
		? std::optional{json_node_get_int(node)} : std::nullopt;
}

std::string member_string(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return {};
	}
	JsonNode *node = json_object_get_member(object, name);
	return json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? std::string{json_node_get_string(node)} : std::string{};
}

Result quit_by_msg() {
	act_when(true, [] {
		sequencer().end_is_coming();
		run_msg("quit", "", "quit");
	});
	if (auto wait = wait_for(false, {.budget_ms = quit_budget_ms})) {
		return *wait;
	}
	check(false, "the compositor ended the run when told to quit");
	return finish();
}

} // namespace harness
