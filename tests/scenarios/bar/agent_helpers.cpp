// The agents panel's world: the usage fixtures, the collector and the card's pixels.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// An ISO-8601 UTC instant `offset` seconds away, for the payload's
// reset times.
std::string iso_in(long long offset) {
	std::time_t when = std::time(nullptr) + offset;
	std::tm utc{};
	if (gmtime_r(&when, &utc) == nullptr) {
		return {};
	}
	return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
		utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
		utc.tm_min, utc.tm_sec);
}

} // namespace

std::string ready_line(Spawned &child, int timeout_s) {
	std::string buffer;
	char chunk[256];
	for (int tick = 0; tick < timeout_s * 2; tick++) {
		pollfd p = {.fd = child.out, .events = POLLIN, .revents = 0};
		if (poll(&p, 1, 500) > 0 && (p.revents & (POLLIN | POLLHUP)) != 0) {
			ssize_t n = read(child.out, chunk, sizeof chunk);
			if (n <= 0) {
				break;
			}
			buffer.append(chunk, static_cast<std::size_t>(n));
			std::size_t newline = buffer.find('\n');
			if (newline != std::string::npos) {
				return buffer.substr(0, newline);
			}
		}
		if (waitpid(child.pid, nullptr, WNOHANG) == child.pid) {
			break;
		}
	}
	return {};
}

std::string day_iso(int offset) {
	std::time_t when = std::time(nullptr)
		+ static_cast<std::time_t>(offset) * 86400;
	std::tm local{};
	if (localtime_r(&when, &local) == nullptr) {
		return {};
	}
	return std::format("{:04d}-{:02d}-{:02d}", local.tm_year + 1900,
		local.tm_mon + 1, local.tm_mday);
}

void write_usage_payload(const char *session, const char *weekly) {
	write_file(std::format("{}/usage-response.json", shot_dir),
		std::format(R"({{"five_hour":{{"utilization":"{}",)"
			R"("resets_at":"{}"}},"seven_day_oauth_apps":)"
			R"({{"utilization":"{}","resets_at":"{}"}}}})",
			session, iso_in(7500), weekly, iso_in(5 * 86400)));
}

std::string usage_line(const std::string &id, const std::string &date,
	const char *clock, const char *provider, const char *model,
	long long input, long long output) {
	return std::format(R"({{"type":"message","id":"{}",)"
		R"("timestamp":"{}T{}","message":{{"role":"assistant",)"
		R"("provider":"{}","model":"{}","usage":{{"input":{},)"
		R"("output":{}}}}}}})"
		"\n", id, date, clock, provider, model, input, output);
}

void drop_usage_fixtures() {
	std::string sessions = scratch_home + "/.omp/agent/sessions/fixture";
	mkdir_p(sessions);
	std::string d5 = day_iso(-5), d3 = day_iso(-3), d2 = day_iso(-2),
		d1 = day_iso(-1), d0 = day_iso(0);
	std::string lines;
	lines += usage_line("a1", d5, "10:00:00", "anthropic",
		"claude-opus-5-5", 1500, 0);
	lines += usage_line("a2", d3, "10:00:00", "anthropic",
		"claude-opus-5-5", 2300000, 0);
	lines += usage_line("a3", d3, "11:00:00", "anthropic",
		"claude-haiku-4-5", 100000, 0);
	lines += usage_line("a4", d2, "12:00:00", "anthropic",
		"claude-opus-5-5", 246132594, 0);
	// The same id again: deduplicated, never counted twice.
	lines += usage_line("a4", d2, "12:00:00", "anthropic",
		"claude-opus-5-5", 246132594, 0);
	lines += usage_line("a5", d1, "13:00:00", "anthropic",
		"claude-opus-5-5", 324632448, 0);
	lines += usage_line("a6", d0, "14:00:00", "anthropic",
		"claude-opus-5-5", 811500, 0);
	// A user turn and a model_usage entry: neither is a message.
	lines += std::format(R"({{"type":"message","id":"x1",)"
		R"("timestamp":"{}T15:00:00","message":{{"role":"user",)"
		R"("provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{{"input":999999}}}}}})"
		"\n", d0);
	lines += std::format(R"({{"type":"model_usage","id":"x2",)"
		R"("timestamp":"{}T15:30:00","provider":"anthropic",)"
		R"("model":"claude-opus-5-5","usage":{{"totalTokens":888888}}}})"
		"\n", d0);
	lines += usage_line("b1", d1, "16:00:00", "openai", "gpt-5.5",
		500000, 0);
	lines += usage_line("b2", d0, "17:00:00", "openai", "gpt-5.5",
		700000, 0);
	lines += usage_line("c1", d0, "18:00:00", "fireworks-300k",
		"glm-5p3-fast", 1250000, 0);
	lines += usage_line("g1", d0, "19:00:00",
		"anthropic-250k-prefer-using-this-one", "claude-opus-5-5", 40000, 0);
	write_file(sessions + "/usage.jsonl", lines);

	// The CLI's saved sign-in: a token an hour from expiring and the plan
	// label the hero's meta line shows.
	std::string claude_dir = scratch_home + "/.claude";
	mkdir_p(claude_dir);
	write_file(claude_dir + "/.credentials.json",
		std::format(R"({{"claudeAiOauth":{{"accessToken":"fixture-token",)"
			R"("expiresAt":{},"rateLimitTier":"max_20x",)"
			R"("subscriptionType":""}}}})",
			static_cast<long long>(std::time(nullptr)) * 1000 + 3600000));

	// A Codex session file: the model on turn_context, the tokens on
	// token_count's last_token_usage, cached already inside input.
	std::string codex_dir = scratch_home + "/.codex/sessions/fixture";
	mkdir_p(codex_dir);
	write_file(codex_dir + "/rollout.jsonl",
		std::format(R"({{"timestamp":"{}T20:00:00","type":"turn_context",)"
			R"("payload":{{"model":"gpt-5.5-codex"}}}})"
			"\n"
			R"({{"timestamp":"{}T20:00:05","type":"response_item",)"
			R"("payload":{{"payload":{{"type":"token_count","info":)"
			R"({{"last_token_usage":{{"input_tokens":5000,)"
			R"("cached_input_tokens":1000,)"
			R"("cache_write_input_tokens":0,)"
			R"("output_tokens":1000}}}}}}}}}})"
			"\n", d1, d1));
}

int codex_runs() {
	return standin_calls(read_file(std::format("{}/codex-standin.log", shot_dir)),
		R"("method":"initialize")");
}

void remove_usage_fixtures() {
	std::error_code ec;
	for (const char *dir : {"/.omp", "/.claude", "/.codex", "/.cache"}) {
		std::filesystem::remove_all(scratch_home + dir, ec);
	}
}

void reload_shell() {
	write_file(shell_config, read_file(shell_config));
}

int colour_count(cairo_surface_t *s, const wlr_box &rect, uint32_t want) {
	if (s == nullptr) {
		return -1;
	}
	int count = 0;
	for (int y = rect.y; y < rect.y + rect.height; y++) {
		for (int x = rect.x; x < rect.x + rect.width; x++) {
			if (pixel_is(s, x, y, want)) {
				count++;
			}
		}
	}
	return count;
}

} // namespace bar
