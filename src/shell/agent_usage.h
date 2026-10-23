#pragma once

// The AI usage collector behind the agents panel (docs/intents/shell/bar.md):
// pure logic over the tools' own records (Claude Code, Codex, pi/omp, opencode)
// and the limits endpoints' payloads. No compositor types and no network, so
// the unit suite covers every rule; the wayland-canvas-agent-usage helper adds
// the environment, the probes and the printing.
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent_usage {

// One message's token split; the total every figure shows is the sum.
struct Usage {
	long long input = 0;
	long long output = 0;
	long long cache_read = 0;
	long long cache_write = 0;
	[[nodiscard]] long long total() const {
		return input + output + cache_read + cache_write;
	}
};

// The two families the helper probes limits for; Claude Code's and Codex's
// own records always belong to them.
inline constexpr std::string_view family_claude = "claude";
inline constexpr std::string_view family_gpt = "gpt";

// `246.1M`, `811.5K`, `1.0K`, `0` — one decimal above a thousand, plain
// below. The one place a token count is formatted: the record carries every
// figure as text.
[[nodiscard]] std::string format_tokens(long long tokens);

// `claude-opus-5-5` → `Opus 5.5`, `gpt-6-astra` → `GPT 6 Astra`: the last
// path segment, the Claude prefix dropped, the numeric run joined into one
// version, the words title-cased.
[[nodiscard]] std::string model_display_name(std::string_view model_id);

// The family a usage entry belongs to, from the provider label (the api label
// when the provider is silent) and, only when neither names a service, the
// model id. Exactly `anthropic` is the Claude subscription; every other label
// roots its own family by its first segment, so a custom Anthropic gateway is
// its own usage and the Claude figures stay Claude Code's own records.
[[nodiscard]] std::string family_of(std::string_view provider,
	std::string_view api, std::string_view model_id);

// The chip label and the hero title of a family.
[[nodiscard]] std::string family_chip(std::string_view family);
[[nodiscard]] std::string family_title(std::string_view family);

// `max_20x` → `Max 20x`, `pro` → `Pro`.
[[nodiscard]] std::string plan_label(std::string_view tier,
	std::string_view subscription);

struct Limit {
	std::string title;   // "Session", "Weekly"
	double percent = -1; // share used, 0 to 1
	std::string resets_at; // ISO-8601, "" when the payload had none
};

// Anthropic's OAuth usage payload: the 5-hour session and the 7-day week,
// their utilisation and reset times. Percentages and fractions are told
// apart by the payload itself: any value at or above 1 is a percentage.
[[nodiscard]] std::vector<Limit> parse_anthropic_limits(
	const std::string &payload);

// The Codex app-server's account/read and account/rateLimits/read
// results: the primary and secondary windows and the plan. An error in
// either result leaves the limits empty.
struct CodexLimits {
	std::vector<Limit> limits;
	std::string plan;
};
[[nodiscard]] CodexLimits parse_codex_limits(const std::string &account_result,
	const std::string &rate_limits_result);

// Where one tool keeps its records. The base is the `env` variable when it
// is set and `home` under $HOME when it is not (`env` is nullptr when no
// variable moves it); `rest` follows the base. `name` keys `Paths::roots`.
struct SourceRoot {
	const char *name;
	const char *env;
	const char *home;
	const char *rest;
};

// The tools the scan reads, in the order their records are counted.
[[nodiscard]] std::vector<SourceRoot> source_roots();

// Every root the scan reads, by source name; the helper fills them from
// `source_roots()` and the environment, the tests from their fixture dirs.
// A source with no root, or an empty one, is skipped.
struct Paths {
	std::map<std::string, std::string> roots;
	std::string cache_file; // the incremental scan cache
};

// The probe's cache: a fresh probe is reused for a short window, so a
// panel opened and shut repeatedly never turns into a request per flick,
// and the last good answer survives a failed probe until its window
// resets.
struct LimitsCache {
	long long fetched_at_ms = 0;
	std::vector<Limit> limits;
};
[[nodiscard]] std::optional<LimitsCache> read_limits_cache(
	const std::string &path);
void write_limits_cache(const std::string &path, const LimitsCache &cache);
// The cached limits whose window has not since reset: a figure past its
// reset describes a period that is over. One with no readable reset time
// stays — an unreadable timestamp is no reason to drop a real number.
[[nodiscard]] std::vector<Limit> open_limits(const std::vector<Limit> &limits);

// The limits side of one provider, as the helper's probes found it:
// `status` is the hero's state line ("" when everything worked) and
// `help` the sign-in card's text.
struct ProviderLimits {
	std::vector<Limit> limits;
	std::string tier;
	std::string status;
	std::string help;
};

struct DayTokens {
	std::string date;   // local YYYY-MM-DD
	std::string label;  // "Sat" … "Today"
	bool today = false;
	long long tokens = 0;
	std::string tokens_text;
};

struct ModelUsage {
	std::string id;
	std::string name;
	Usage usage;
	[[nodiscard]] long long total() const { return usage.total(); }
	std::string total_text;
};

struct ProviderRecord {
	std::string id;    // the family key
	std::string chip;  // the switch's label
	std::string title; // the hero's title
	std::vector<Limit> limits;
	std::string tier;
	std::string status;
	std::string help;
	std::vector<DayTokens> days;   // the last 7, oldest first, today last
	std::string week_text;         // the seven days' tokens, formatted
	std::vector<ModelUsage> models; // heaviest first
	long long total_prompts = 0;
	long long total_sessions = 0;
};

struct Record {
	std::vector<ProviderRecord> providers; // Claude and GPT first, then the rest
	std::string updated_at;
};

// Scans every root, reusing the per-file cache for files whose mtime and
// size still match (a refresh reparses only what changed), and merges the
// limits into their providers. A provider appears when it has usage or
// limits to show.
[[nodiscard]] Record collect(const Paths &paths,
	const std::map<std::string, ProviderLimits> &limits);

// The record as the panel reads it.
[[nodiscard]] std::string record_json(const Record &record);

} // namespace agent_usage
