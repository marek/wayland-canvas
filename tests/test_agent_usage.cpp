// Unit tests for the AI usage collector (src/shell/agent_usage.cpp) over
// fixture files the test itself writes under $CANVAS_TMP: the counting
// rules for Claude transcripts, Codex session files, pi/omp session logs
// and the opencode database; the family rule; day buckets at local
// midnight; model display names; number formatting; limits payloads; and
// the incremental cache (a file whose bytes change behind an unchanged
// stat keeps its counted contribution — proof a refresh reparses only
// what changed).
#include "check.h"
#include "shell/agent_usage.h"

#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <sys/time.h>
#include <ctime>
#include <vector>

#include "core/json.h"

namespace {

using namespace agent_usage;

using unit::check;

std::string root; // $CANVAS_TMP/agent-usage-unit

void write_file(const std::string &path, std::string_view text) {
	std::filesystem::path target(path);
	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);
	FILE *f = std::fopen(path.c_str(), "w");
	if (f == nullptr) {
		check(false, std::format("cannot write {}", path));
		return;
	}
	std::fwrite(text.data(), 1, text.size(), f);
	std::fclose(f);
}

// The local date `offset` days from today: the collector buckets at local
// midnight, so the fixtures must speak local dates.
std::string date_offset(int offset) {
	std::time_t shifted = std::time(nullptr)
		+ static_cast<std::time_t>(offset) * 86400;
	std::tm local{};
	localtime_r(&shifted, &local);
	return std::format("{:04d}-{:02d}-{:02d}", local.tm_year + 1900,
		local.tm_mon + 1, local.tm_mday);
}

// Joins raw JSON chunks with the dates that slot between them: the
// fixtures stay literal, with none of std::format's brace escaping.
std::string join(std::initializer_list<std::string_view> pieces) {
	std::string out;
	for (std::string_view piece : pieces) {
		out.append(piece);
	}
	return out;
}

void restore_mtime(const std::string &path, const struct stat &st) {
	timespec times[2] = {st.st_atim, st.st_mtim};
	utimensat(AT_FDCWD, path.c_str(), times, 0);
}

// ---------------------------------------------------------------- fixture

struct Fixture {
	std::string claude_dir;
	std::string codex_home;
	std::string sessions;   // the omp root
	std::string opencode_db;
	std::string cache;
};

// The fixture world, mirroring the shapes the real tools write:
// - Claude transcripts, including a message id that appears twice;
// - pi/omp session files with every provider family this machine shows;
// - a Codex session file with turn_context models and token counts;
// - an opencode database whose rows land in the same families.
Fixture make_fixture() {
	Fixture fixture;
	fixture.claude_dir = root + "/claude";
	fixture.codex_home = root + "/codex";
	fixture.sessions = root + "/sessions";
	fixture.opencode_db = root + "/opencode/opencode.db";
	fixture.cache = root + "/cache/agent-usage.json";


	std::string d2 = date_offset(-2);
	std::string d1 = date_offset(-1);
	std::string d0 = date_offset(0);

	// Claude Code transcripts: message.usage in the CLI's own snake_case,
	// one assistant message whose id repeats across two files. The second
	// copy is naive-local (23:30 on day -1), the rest Z-stamped.
	write_file(fixture.claude_dir + "/projects/home/one.jsonl", join({
		R"({"type":"assistant","timestamp":")", d2, R"(T10:00:00.000Z",)"
		R"("sessionId":"s-one","message":{"id":"msg_1","role":"assistant",)"
		R"("model":"claude-opus-5-5","usage":{"input_tokens":200000,)"
		R"("output_tokens":1000,"cache_read_input_tokens":45900000,)"
		R"("cache_creation_input_tokens":100000}}})", "\n",
		R"({"type":"user","timestamp":")", d2, R"(T10:01:00Z","message":)"
		R"({"role":"user","usage":{"input_tokens":999}}})", "\n",
	}));
	write_file(fixture.claude_dir + "/projects/home/two.jsonl", join({
		R"({"type":"assistant","timestamp":")", d1, R"(T23:30:00",)"
		R"("sessionId":"s-two","message":{"id":"msg_2","role":"assistant",)"
		R"("model":"claude-sonnet-4-6","usage":{"inputTokens":1500,)"
		R"("outputTokens":500,"cacheReadInputTokens":1000,)"
		R"("cacheCreationInputTokens":0}}})", "\n",
		// The same message id again: deduplicated, never counted twice.
		R"({"type":"assistant","timestamp":")", d2, R"(T09:00:00Z",)"
		R"("message":{"id":"msg_1","role":"assistant",)"
		R"("model":"claude-opus-5-5","usage":{"input_tokens":200000,)"
		R"("output_tokens":1000,"cache_read_input_tokens":45900000,)"
		R"("cache_creation_input_tokens":100000}}})", "\n",
	}));

	// omp sessions: every provider family, plus lines that must not count
	// (a user turn, a duplicate id, a model_usage entry).
	write_file(fixture.sessions + "/a.jsonl", join({
		R"({"type":"message","id":"m1","timestamp":")", d0,
		R"(T08:00:00.000Z","message":{"role":"assistant",)"
		R"("provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{"input":4,"output":5,"cacheRead":991,"cacheWrite":0}}})", "\n",
		R"({"type":"message","id":"m1","timestamp":")", d0,
		R"(T08:00:00.000Z","message":{"role":"assistant",)"
		R"("provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{"input":4,"output":5,"cacheRead":991,"cacheWrite":0}}})", "\n",
		R"({"type":"message","id":"m2","timestamp":")", d0,
		R"(T09:00:00Z","message":{"role":"user","provider":"anthropic",)"
		R"("model":"claude-opus-5-5","usage":{"input":400}}})", "\n",
		R"({"type":"model_usage","id":"m3","timestamp":")", d0,
		R"(T09:30:00Z","provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{"input":7,"output":8,"totalTokens":15}})", "\n",
	}));
	write_file(fixture.sessions + "/b.jsonl", join({
		R"({"type":"message","id":"g1","timestamp":")", d1,
		R"(T12:00:00Z","message":{"role":"assistant","provider":"openai",)"
		R"("api":"openai-responses","model":"gpt-5.5",)"
		R"("usage":{"input":500,"output":100,"cacheRead":300,"cacheWrite":100}}})", "\n",
		R"({"type":"message","id":"g2","timestamp":")", d1,
		R"(T12:05:00Z","message":{"role":"assistant","provider":"openai-1m",)"
		R"("model":"gpt-6-astra","usage":{"input":600,"output":200,)"
		R"("cacheRead":400,"cacheWrite":200}}})", "\n",
		// A totalTokens-only entry: everything reads as input.
		R"({"type":"message","id":"g3","timestamp":")", d1,
		R"(T12:10:00Z","message":{"role":"assistant",)"
		R"("provider":"openai-flex","model":"gpt-5.5",)"
		R"("usage":{"totalTokens":700}}})", "\n",
		R"({"type":"message","id":"f1","timestamp":")", d0,
		R"(T13:00:00Z","message":{"role":"assistant",)"
		R"("provider":"fireworks-300k",)"
		R"("model":"fireworks:accounts/fireworks/routers/glm-5p3-fast",)"
		R"("usage":{"input":100,"output":50,"cacheRead":50,"cacheWrite":0}}})", "\n",
		R"({"type":"message","id":"x1","timestamp":")", d1,
		R"(T14:00:00Z","message":{"role":"assistant",)"
		R"("provider":"anthropic-250k-prefer-using-this-one",)"
		R"("model":"claude-opus-5-5","usage":{"input":10,"output":10,)"
		R"("cacheRead":10,"cacheWrite":10}}})", "\n",
		R"({"type":"message","id":"t1","timestamp":")", d1,
		R"(T15:00:00Z","message":{"role":"assistant",)"
		R"("provider":"acme-gateway","model":"claude-opus-5",)"
		R"("usage":{"input":1,"output":2,"cacheRead":3,"cacheWrite":4}}})", "\n",
		R"({"type":"message","id":"z1","timestamp":")", d0,
		R"(T16:00:00Z","message":{"role":"assistant","provider":"meta",)"
		R"("model":"muse-spark-1.3","usage":{"input":40,"output":60}}})", "\n",
	}));
	// A pi-root twin: same shape, another directory.
	write_file(root + "/pi-sessions/c.jsonl", join({
		R"({"type":"message","id":"p9","timestamp":")", d0,
		R"(T10:00:00Z","message":{"role":"assistant",)"
		R"("provider":"openai-codex","model":"codex-mini-latest",)"
		R"("usage":{"input":80,"output":20}}})", "\n",
	}));

	// A Codex CLI session: the model rides on turn_context, the tokens on
	// token_count's last_token_usage, cached already inside input.
	write_file(fixture.codex_home + "/sessions/2026/10/16/rollout.jsonl", join({
		R"({"timestamp":")", d1, R"(T11:00:00Z","type":"turn_context",)"
		R"("payload":{"model":"gpt-5.5-codex"}})", "\n",
		R"({"timestamp":")", d1, R"(T11:00:05Z","type":"response_item",)"
		R"("payload":{"payload":{"type":"token_count","info":)"
		R"({"last_token_usage":{"input_tokens":1000,)"
		R"("cached_input_tokens":400,"cache_write_input_tokens":100,)"
		R"("output_tokens":500}}}}})", "\n",
		R"({"timestamp":")", d1, R"(T11:05:00Z","type":"turn_context",)"
		R"("payload":{"model":"gpt-6-astra"}})", "\n",
		R"({"timestamp":")", d1, R"(T11:05:05Z","type":"response_item",)"
		R"("payload":{"payload":{"type":"token_count","info":)"
		R"({"last_token_usage":{"input_tokens":2000,)"
		R"("cached_input_tokens":0,"cache_write_input_tokens":0,)"
		R"("output_tokens":300}}}}})", "\n",
	}));

	// The opencode database: rows whose providers land in the same
	// families as the session logs.
	{
		sqlite3 *db = nullptr;
		std::filesystem::path db_path(fixture.opencode_db);
		std::error_code open_ec;
		std::filesystem::remove(db_path, open_ec); // fresh rows every call
		std::filesystem::create_directories(db_path.parent_path());
		sqlite3_open(fixture.opencode_db.c_str(), &db);
		char *error = nullptr;
		sqlite3_exec(db, "CREATE TABLE message (session_id TEXT, data TEXT)",
			nullptr, nullptr, &error);
		auto insert = [&](const char *session, const std::string &data) {
			std::string sql = "INSERT INTO message VALUES ('" + std::string{session}
				+ "', '" + data + "')";
			sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
		};
		insert("s1", R"({"role":"assistant","providerID":"anthropic",)"
			R"("modelID":"claude-opus-5-5","tokens":{"input":11,)"
			R"("output":22,"cache":{"read":33,"write":44}},)"
			R"("time":{"created":1730000000000}})");
		insert("s2", R"({"role":"assistant","providerID":"openai",)"
			R"("modelID":"openai/gpt-5.5","tokens":{"input":60,)"
			R"("output":40,"reasoning":20,"cache":{"read":10,"write":10}}})");
		// no "time" member: no day bucket, still counted

		insert("s3", R"({"role":"assistant","providerID":"fireworks-300k",)"
			R"("modelID":"fireworks/glm-5p3-fast","tokens":{"input":5,)"
			R"("output":5}})");
		insert("s4", R"({"role":"user","providerID":"anthropic"})");
		if (error != nullptr) {
			check(false, std::format("opencode fixture: {}", error));
			sqlite3_free(error);
		}
		sqlite3_close(db);
	}
	return fixture;
}

Record collect_fixture(const Fixture &fixture,
	const std::map<std::string, ProviderLimits> &limits = {}) {
	Paths paths{.roots = {{"claude", fixture.claude_dir},
			      {"codex", fixture.codex_home},
			      {"pi", root + "/pi-sessions"},
			      {"omp", fixture.sessions},
			      {"opencode", fixture.opencode_db}},
		.cache_file = fixture.cache};
	return collect(paths, limits);
}

const ProviderRecord *find_provider(const Record &record,
	const std::string &id) {
	for (const ProviderRecord &provider : record.providers) {
		if (provider.id == id) {
			return &provider;
		}
	}
	return nullptr;
}

long long day_tokens(const ProviderRecord &provider, int offset) {
	if (provider.days.empty()) {
		return -1;
	}
	return provider.days[static_cast<std::size_t>(6 - offset)].tokens;
}

// ------------------------------------------------------------- the checks

void test_display_names() {
	check(format_tokens(0) == "0", "0 formats as 0");
	check(format_tokens(999) == "999", "999 formats as 999");
	check(format_tokens(1500) == "1.5K", "1500 formats as 1.5K");
	check(format_tokens(811500) == "811.5K", "811500 formats as 811.5K");
	check(format_tokens(246100000) == "246.1M", "246100000 formats as 246.1M");
	check(format_tokens(1200000000) == "1.2B", "1200000000 formats as 1.2B");

	check(model_display_name("claude-opus-5-5") == "Opus 5.5",
		"claude-opus-5-5 is Opus 5.5");
	check(model_display_name("claude-sonnet-4-6") == "Sonnet 4.6",
		"claude-sonnet-4-6 is Sonnet 4.6");
	check(model_display_name("gpt-5.5") == "GPT 5.5", "gpt-5.5 is GPT 5.5");
	check(model_display_name("gpt-6-astra") == "GPT 6 Astra",
		"gpt-6-astra is GPT 6 Astra");
	check(model_display_name("gpt-5.5-codex") == "GPT 5.5 Codex",
		"gpt-5.5-codex is GPT 5.5 Codex");
	check(model_display_name("codex-mini-latest") == "Codex Mini Latest",
		"codex-mini-latest is Codex Mini Latest");
	check(model_display_name(
			"fireworks:accounts/fireworks/routers/glm-5p3-fast")
			== "GLM 5p3 Fast",
		"the router-qualified GLM id is GLM 5p3 Fast");
	check(model_display_name("openai/gpt-5.5") == "GPT 5.5",
		"a provider-qualified id drops its prefix");
	check(model_display_name("muse-spark-1.3") == "Muse Spark 1.3",
		"muse-spark-1.3 is Muse Spark 1.3");
	check(model_display_name("claude-opus-4-1-20250805") == "Opus 4.1",
		"a dated snapshot suffix drops away");
	check(model_display_name("") == "Unknown", "no model is Unknown");
}

void test_family_rule() {
	check(family_of("anthropic", "", "claude-opus-5-5") == "claude",
		"the exact anthropic provider is the Claude subscription");
	check(family_of("anthropic-250k-prefer-using-this-one", "",
			"claude-opus-5-5") == "anthropic",
		"a custom Anthropic gateway is its own family, not the subscription");
	check(family_of("anthropic-1m", "", "claude-fable-5-1") == "anthropic",
		"every anthropic-* gateway groups under one family key");
	check(family_of("openai", "openai-responses", "gpt-5.5") == "gpt",
		"the openai provider is GPT");
	check(family_of("openai-1m", "", "gpt-6-astra") == "gpt",
		"openai-1m is GPT");
	check(family_of("openai-codex", "openai-codex", "") == "gpt",
		"openai-codex is GPT");
	check(family_of("openai-flex", "", "gpt-5.5") == "gpt",
		"openai-flex is GPT");
	check(family_of("fireworks-300k", "", "glm-5p3-fast") == "fireworks",
		"fireworks-300k roots the fireworks family");
	check(family_of("acme-gateway", "", "claude-opus-5") == "acme",
		"a claude model behind a gateway is the gateway's family");
	check(family_of("meta", "openai-responses", "muse-spark-1.3") == "meta",
		"the meta provider is meta even on an OpenAI-style api");
	check(family_of("typesafe", "", "") == "typesafe",
		"an unknown label roots its own family");
	check(family_of("", "", "claude-fable-5-1") == "claude",
		"with no labels, a claude model is Claude");
	check(family_of("", "openai-codex", "") == "gpt",
		"with no provider, the api label decides");
	check(family_of("", "", "gemini-2.5-pro") == "gemini",
		"a gemini model with no labels is Gemini");
	check(family_of("", "", "mystery-model") == "other",
		"anything else is Other");
	check(family_chip("claude") == "Claude" && family_title("claude")
			== "Claude Code",
		"the Claude family chips as Claude, titles as Claude Code");
	check(family_chip("gpt") == "GPT" && family_title("gpt") == "GPT",
		"the GPT family is GPT both ways");
	check(family_chip("fireworks") == "Fireworks",
		"fireworks chips as Fireworks");
}

void test_plan_label() {
	check(plan_label("max_20x", "") == "Max 20x", "max_20x is Max 20x");
	check(plan_label("claude_max_5x_2026", "") == "Max 5x",
		"the tier names its multiple anywhere in the string");
	check(plan_label("", "pro") == "Pro", "a subscription type is capitalized");
	check(plan_label("something-else", "pro") == "Pro",
		"a tier without a multiple falls back to the subscription");
	check(plan_label("", "") == "", "no plan is empty");
}

void test_anthropic_limits() {
	// Percentages: any value at or above 1 makes the whole payload
	// percent-scaled, so a lone 1.0 is 1%.
	std::string payload = join({
		R"({"five_hour":{"utilization":"37.0","resets_at":)",
		std::to_string(std::time(nullptr) + 7500),
		R"(},"seven_day_oauth_apps":{"utilization":"68.4",)"
		R"("resets_at":")", date_offset(3), R"(T21:00:00Z"}})",
	});
	std::vector<Limit> limits = parse_anthropic_limits(payload);
	check(limits.size() == 2, "both windows parsed");
	if (limits.size() == 2) {
		check(limits[0].title == "Session" && limits[1].title == "Weekly",
			"the windows are titled Session and Weekly");
		check(std::abs(limits[0].percent - 0.37) < 0.0001
			&& std::abs(limits[1].percent - 0.684) < 0.0001,
			"percentages are shares, not numbers");
		check(!limits[0].resets_at.empty() && !limits[1].resets_at.empty(),
			"both resets normalize");
	}
	// Fractions, not percentages, when every value is below 1.
	std::vector<Limit> fractions = parse_anthropic_limits(
		R"({"five_hour":{"utilization":0.5},"seven_day":{"utilization":0.25}})");
	check(fractions.size() == 2 && std::abs(fractions[0].percent - 0.5) < 0.0001
			&& std::abs(fractions[1].percent - 0.25) < 0.0001,
		"a fraction payload stays fractional");
	check(parse_anthropic_limits("not json").empty(),
		"a bad payload parses to no limits");
	check(parse_anthropic_limits(R"({"seven_day":{}})").empty(),
		"a window without utilisation is skipped");
}

void test_codex_limits() {
	std::string account = R"({"id":2,"result":{"account":{"planType":"pro"}}})";
	std::string limits = join({
		R"({"id":3,"result":{"rateLimits":{"planType":"Pro",)"
		R"("primary":{"usedPercent":41.5,"windowDurationMins":300,)"
		R"("resetsAt":)", std::to_string(std::time(nullptr) + 3600),
		R"(},"secondary":{"usedPercent":77.0,)"
		R"("windowDurationMins":10080,"resetsAt":)",
		std::to_string(std::time(nullptr) + 5 * 86400), R"(}}}})",
	});
	CodexLimits parsed = parse_codex_limits(account, limits);
	check(parsed.plan == "Pro", "the plan comes from the rate limits first");
	check(parsed.limits.size() == 2, "both windows parsed");
	if (parsed.limits.size() == 2) {
		check(parsed.limits[0].title == "Session"
			&& parsed.limits[1].title == "Weekly",
			"a 5-hour window is Session, a 10080-minute one Weekly");
		check(std::abs(parsed.limits[0].percent - 0.415) < 0.0001
			&& std::abs(parsed.limits[1].percent - 0.77) < 0.0001,
			"usedPercent becomes a share");
	}
	// The plan falls back to the account when the limits carry none.
	CodexLimits fallback = parse_codex_limits(account,
		R"({"id":3,"result":{"rateLimits":{"primary":{"usedPercent":10,"windowDurationMins":300}}}})");
	check(fallback.plan == "pro", "the account's planType stands in");
}

void test_collection() {
	Fixture fixture = make_fixture();
	Record record = collect_fixture(fixture);

	// Claude: the transcript messages, the omp anthropic entry and the
	// opencode anthropic row, with the repeated message id counted once,
	// the user turn ignored and the model_usage line (not a message)
	// ignored.
	const ProviderRecord *claude = find_provider(record, "claude");
	check(claude != nullptr, "the Claude provider is present");
	if (claude != nullptr) {
		long long msg1 = 200000 + 1000 + 45900000 + 100000; // counted once
		long long sonnet = 1500 + 500 + 1000;
		long long m1 = 4 + 5 + 991;         // the omp entry
		long long s1 = 11 + 22 + 33 + 44;   // the opencode row, 2024
		long long opus = msg1 + m1 + s1;
		check(claude->models.size() == 2
			&& claude->models[0].id == "claude-opus-5-5"
			&& claude->models[0].name == "Opus 5.5"
			&& claude->models[0].total() == opus
			&& claude->models[1].id == "claude-sonnet-4-6"
			&& claude->models[1].total() == sonnet,
			std::format("Claude's models are the deduplicated sums "
				"(opus {} of {})", claude->models.empty() ? 0LL
					: claude->models[0].total(), opus));
		// msg_1's Z timestamp on day -2 at 10:00Z is early morning local
		// on day -2; msg_2's naive local timestamp on day -1 at 23:30
		// stays on day -1; the opencode row is 2024, outside the week.
		check(day_tokens(*claude, 2) == msg1,
			"day -2 holds the transcript's Z-stamped opus message");
		check(day_tokens(*claude, 1) == sonnet,
			"day -1 holds the sonnet message at local 23:30");
		check(day_tokens(*claude, 0) == m1,
			"today holds the omp anthropic message");
		check(claude->days.size() == 7 && claude->days.back().today
			&& claude->days.back().tokens_text == "1.0K",
			"seven days ending in a flagged Today");
	}

	// GPT: the Codex file, the openai* omp entries, the pi codex entry
	// and the opencode openai row, with cached tokens kept out of input.
	const ProviderRecord *gpt = find_provider(record, "gpt");
	check(gpt != nullptr, "the GPT provider is present");
	if (gpt != nullptr) {
		long long codex55 = (1000 - 400 - 100) + 400 + 100 + 500;
		long long codexAstra = 2000 + 300;
		long long ompG55 = 500 + 100 + 300 + 100;
		long long flex = 700;
		long long opencode = 60 + 40 + 20 + 10 + 10;
		long long gpt55 = ompG55 + flex + opencode;
		long long astra = codexAstra + 1400;
		long long mini = 80 + 20;
		check(gpt->models.size() == 4
			&& gpt->models[0].id == "gpt-6-astra"
			&& gpt->models[0].total() == astra
			&& gpt->models[1].id == "gpt-5.5"
			&& gpt->models[1].total() == gpt55
			&& gpt->models[2].id == "gpt-5.5-codex"
			&& gpt->models[2].total() == codex55
			&& gpt->models[3].name == "Codex Mini Latest",
			std::format("GPT's models merge every OpenAI source, heaviest "
				"first ({} rows)", gpt->models.size()));
		check(gpt->models[1].usage.input == 500 + 700 + 60
			&& gpt->models[1].usage.cache_read == 300 + 10
			&& gpt->models[1].usage.cache_write == 100 + 10,
			"the cached tokens keep their split, out of input");
		check(day_tokens(*gpt, 1) == codex55 + codexAstra + ompG55 + flex
				+ 1400,
			"day -1 holds the Codex file and the day-old omp entries");
		// The opencode row without a time member lands on today, the
		// same fallback the tools' own records use.
		check(day_tokens(*gpt, 0) == mini + opencode,
			"today holds the pi codex entry and the timeless opencode row");
	}

	// Every other family with usage shows, token-only.
	const ProviderRecord *fireworks = find_provider(record, "fireworks");
	check(fireworks != nullptr && fireworks->limits.empty()
			&& fireworks->title == "Fireworks" && fireworks->chip == "Fireworks",
		"the fireworks family shows with no limits");
	if (fireworks != nullptr) {
		// the omp entry (router-qualified) plus the opencode row
		// (provider-qualified): one merged model row.
		check(fireworks->models.size() == 1
			&& fireworks->models[0].total() == 210
			&& fireworks->models[0].name == "GLM 5p3 Fast",
			std::format("fireworks merges its omp and opencode usage "
				"({} rows)", fireworks->models.size()));
	}
	const ProviderRecord *gateway = find_provider(record, "anthropic");
	check(gateway != nullptr && gateway->models.size() == 1
			&& gateway->models[0].total() == 40,
		"the custom Anthropic gateway's claude usage is its own tab");
	check(gateway != nullptr && gateway->title == "Claude (gateway)"
		&& gateway->chip == "Claude GW",
		"the gateway family is Claude (gateway), chipped Claude GW");
	check(find_provider(record, "acme") != nullptr,
		"the acme gateway family shows");
	check(find_provider(record, "meta") != nullptr,
		"the meta family shows");
	check(find_provider(record, "other") == nullptr,
		"nothing unclassifiable was invented");

	// Provider order: Claude, GPT, then the rest by their week.
	check(record.providers.size() >= 4
		&& record.providers[0].id == "claude"
		&& record.providers[1].id == "gpt",
		"Claude and GPT lead, the rest follow");

	// The limits merge in.
	std::map<std::string, ProviderLimits> limits;
	limits["gpt"] = ProviderLimits{
		.limits = {Limit{.title = "Session", .percent = 0.5,
			.resets_at = "2999-01-01T00:00:00Z"}},
		.tier = "Pro",
	};
	Record merged = collect_fixture(fixture, limits);
	const ProviderRecord *limited = find_provider(merged, "gpt");
	check(limited != nullptr && limited->limits.size() == 1
			&& limited->tier == "Pro",
		"the probed limits merge into the provider");
	// A signed-out provider keeps its card text and its figures.
	limits["gpt"] = ProviderLimits{.status = "Waiting for auth",
		.help = "Run `codex login` to authenticate."};
	Record signed_out = collect_fixture(fixture, limits);
	const ProviderRecord *card = find_provider(signed_out, "gpt");
	check(card != nullptr && card->status == "Waiting for auth"
			&& !card->days.empty(),
		"a signed-out provider keeps its figures beside its card");
}

void test_cache_reuse() {
	Fixture fixture = make_fixture();
	// Drop any cache an earlier check left, then collect: the scan caches
	// every file's contribution keyed by its stat.
	std::error_code ec;
	std::filesystem::remove(fixture.cache, ec);
	Record first = collect_fixture(fixture);
	check(!first.providers.empty(), "the first scan reads the fixture");

	// Replace one file's bytes with garbage while holding its stat still:
	// a collector that reparses it would lose the numbers; the cache keeps
	// them.
	std::string target = fixture.sessions + "/a.jsonl";
	struct stat st{};
	if (stat(target.c_str(), &st) != 0) {
		check(false, "the cache-reuse fixture file went missing");
		return;
	}
	write_file(target, std::string(static_cast<std::size_t>(st.st_size),
		'x'));
	restore_mtime(target, st);
	Record second = collect_fixture(fixture);
	bool kept = false;
	for (const ProviderRecord &provider : second.providers) {
		if (provider.id == "claude") {
			kept = day_tokens(provider, 0) == 1000;
		}
	}
	check(kept, "an unchanged-stat file is not reparsed (the cache "
		"answer stands)");

	// A genuinely changed file is reparsed and the cache follows.
	write_file(fixture.sessions + "/a.jsonl",
		R"({"type":"message","id":"m9","timestamp":")"
		+ date_offset(0) + R"(T18:00:00Z","message":{"role":"assistant",)"
		R"("provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{"input":500,"output":500}}})"
		"\n");
	Record third = collect_fixture(fixture);
	bool refreshed = false;
	for (const ProviderRecord &provider : third.providers) {
		if (provider.id == "claude") {
			// The old entry is gone; the new one carries the same total.
			refreshed = day_tokens(provider, 0) == 1000;
		}
	}
	check(refreshed, "a changed file is reparsed (its new contribution "
		"shows)");
}

void test_midnight_buckets() {
	// Two instants an hour apart, one either side of local midnight, both
	// written with explicit Z offsets: the collector must land them on
	// different local dates. 03:30Z and 04:30Z straddle midnight in the
	// fixed UTC-4 zone main() sets, so one lands a local day before the other.
	std::string late = date_offset(-1);
	write_file(root + "/midnight/a.jsonl", join({
		R"({"type":"message","id":"w1","timestamp":")", late,
		R"(T03:30:00Z","message":{"role":"assistant",)"
		R"("provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{"input":100,"output":100}}})", "\n",
		R"({"type":"message","id":"w2","timestamp":")", late,
		R"(T04:30:00Z","message":{"role":"assistant",)"
		R"("provider":"anthropic","model":"claude-opus-5-5",)"
		R"("usage":{"input":200,"output":200}}})", "\n",
	}));
	Paths paths{.roots = {{"pi", root + "/midnight"}},
		.cache_file = root + "/midnight-cache.json"};
	Record record = collect(paths, {});
	const ProviderRecord *claude = find_provider(record, "claude");
	check(claude != nullptr, "the midnight fixture produced a provider");
	if (claude != nullptr) {
		long long d1 = day_tokens(*claude, 1);
		long long d2 = day_tokens(*claude, 2);
		long long d0 = day_tokens(*claude, 0);
		check(d2 == 200 && d1 == 400 && d0 == 0,
			std::format("instants either side of local midnight land on "
				"different days ({} {} {})", d2, d1, d0));
	}
}

// An opencode row whose session_id or data column is NULL is skipped by
// itself; the rows around it count.
void test_opencode_null_columns() {
	std::string db_path = root + "/opencode-null/opencode.db";
	std::filesystem::create_directories(
		std::filesystem::path(db_path).parent_path());
	std::filesystem::remove(db_path);
	sqlite3 *db = nullptr;
	sqlite3_open(db_path.c_str(), &db);
	std::string message = R"({"role":"assistant","providerID":"anthropic",)"
		R"("modelID":"claude-opus-5-5","tokens":{"input":)";
	char *error = nullptr;
	std::string sql = "CREATE TABLE message (session_id TEXT, data TEXT);"
		"INSERT INTO message VALUES ('s1', '" + message + "7,\"output\":3}}');"
		"INSERT INTO message VALUES (NULL, '" + message
			+ "100,\"output\":100}}');"
		"INSERT INTO message VALUES ('s2', NULL);"
		"INSERT INTO message VALUES (NULL, NULL);"
		"INSERT INTO message VALUES ('s3', '" + message + "20,\"output\":5}}');";
	sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
	if (error != nullptr) {
		check(false, std::format("opencode NULL fixture: {}", error));
		sqlite3_free(error);
	}
	sqlite3_close(db);
	Paths paths{.roots = {{"opencode", db_path}},
		.cache_file = root + "/opencode-null-cache.json"};
	Record record = collect(paths, {});
	const ProviderRecord *claude = find_provider(record, "claude");
	check(claude != nullptr, "the rows around a NULL row produce a provider");
	if (claude != nullptr) {
		long long today = day_tokens(*claude, 0);
		check(today == 7 + 3 + 20 + 5,
			std::format("rows with a NULL session_id or data are skipped "
				"and the rest count ({})", today));
	}
}

// The helper resolves one root per table row, so the rows are the whole
// list of tools the scan reads and each is named once.
void test_source_roots() {
	std::vector<std::string> names;
	bool described = true;
	for (const SourceRoot &source : source_roots()) {
		names.emplace_back(source.name);
		described = described && source.home != nullptr && *source.home != '\0'
			&& source.rest != nullptr;
	}
	check(names == std::vector<std::string>{"claude", "pi", "omp", "codex",
			"opencode"},
		"the table lists Claude, pi, omp, Codex and opencode, in the order "
		"a message is first counted");
	check(described, "every source says where its root is under $HOME");
}

// A source with no root, or an empty one, is skipped: nothing is read and
// no provider appears.
void test_sources_without_roots() {
	Paths none{.cache_file = root + "/no-roots-cache.json"};
	check(collect(none, {}).providers.empty(), "no roots, no providers");
	Paths blank{.roots = {{"claude", ""}, {"codex", ""}, {"pi", ""},
			      {"omp", ""}, {"opencode", ""}},
		.cache_file = root + "/blank-roots-cache.json"};
	check(collect(blank, {}).providers.empty(),
		"empty roots, no providers");
}

void test_record_json() {
	Fixture fixture = make_fixture();
	std::string json = record_json(collect_fixture(fixture));
	JsonParserPtr parser = parse_json(json);
	JsonObject *object = root_object(parser);
	check(object != nullptr, "the record is valid JSON");
	if (object == nullptr) {
		return;
	}
	check(json_object_has_member(object, "updatedAt")
		&& json_object_has_member(object, "providers"),
		"the record carries its stamp and providers");
	JsonArray *providers = json_object_get_array_member(object, "providers");
	check(json_array_get_length(providers) >= 4,
		"the providers array holds every family with usage");
	JsonObject *first = json_array_get_object_element(providers, 0);
	check(std::string{json_object_get_string_member(first, "title")}
			== "Claude Code",
		"the first provider is the Claude tab");
	JsonArray *days = json_object_get_array_member(first, "days");
	check(json_array_get_length(days) == 7
		&& json_object_get_boolean_member(
			json_array_get_object_element(days, 6), "today"),
		"seven days with today flagged last");
	JsonObject *today = json_array_get_object_element(days, 6);
	check(std::string{json_object_get_string_member(today, "label")}
			== "Today",
		"the last day labels as Today");
}

} // namespace

int main() {
	const char *env = std::getenv("CANVAS_TMP");
	root = env != nullptr && *env != '\0' ? env : "tmp";
	root += "/agent-usage-unit";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);
	// A fixed-offset zone (UTC-4 all year, no zoneinfo) keeps the midnight
	// arithmetic and date_offset's day steps the same in every season.
	setenv("TZ", "<-04>4", 1);
	tzset();

	test_display_names();
	test_family_rule();
	test_plan_label();
	test_anthropic_limits();
	test_codex_limits();
	test_collection();
	test_cache_reuse();
	test_midnight_buckets();
	test_opencode_null_columns();
	test_source_roots();
	test_sources_without_roots();
	test_record_json();

	return unit::summary();
}
