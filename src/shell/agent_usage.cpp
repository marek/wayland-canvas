#include "shell/agent_usage.h"

#include <json-glib/json-glib.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace agent_usage {
namespace {

struct ParserDelete {
	void operator()(JsonParser *parser) const {
		if (parser != nullptr) {
			g_object_unref(parser);
		}
	}
};

using ParserPtr = std::unique_ptr<JsonParser, ParserDelete>;

ParserPtr make_parser() {
	return ParserPtr(json_parser_new());
}

// Parses one JSON document; null when it is not valid JSON.
ParserPtr parse_json(const std::string &text) {
	ParserPtr parser = make_parser();
	if (!json_parser_load_from_data(parser.get(), text.c_str(),
		    static_cast<gssize>(text.size()), nullptr)) {
		return nullptr;
	}
	return parser;
}

JsonObject *root_object(const ParserPtr &parser) {
	if (parser == nullptr) {
		return nullptr;
	}
	JsonNode *root = json_parser_get_root(parser.get());
	return root != nullptr && json_node_get_node_type(root) == JSON_NODE_OBJECT
		? json_node_get_object(root) : nullptr;
}

JsonObject *obj_member(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return nullptr;
	}
	JsonNode *node = json_object_get_member(object, name);
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_OBJECT
		? json_node_get_object(node) : nullptr;
}

// A member as a string: strings travel as they are, whole numbers become
// digits (a timestamp or a token count stored as a number).
std::string str_member(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return {};
	}
	JsonNode *node = json_object_get_member(object, name);
	if (node == nullptr || json_node_get_node_type(node) != JSON_NODE_VALUE) {
		return {};
	}
	switch (json_node_get_value_type(node)) {
	case G_TYPE_STRING:
		return json_node_get_string(node) != nullptr
			? std::string{json_node_get_string(node)} : std::string{};
	case G_TYPE_INT64:
		return std::to_string(json_node_get_int(node));
	case G_TYPE_DOUBLE: {
		double value = json_node_get_double(node);
		return std::isfinite(value) ? std::to_string(std::llround(value))
					   : std::string{};
	}
	default:
		return {};
	}
}

// A member as a whole number, from an int, a float or numeric text.
long long int_member(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return 0;
	}
	JsonNode *node = json_object_get_member(object, name);
	if (node == nullptr || json_node_get_node_type(node) != JSON_NODE_VALUE) {
		return 0;
	}
	switch (json_node_get_value_type(node)) {
	case G_TYPE_INT64:
		return json_node_get_int(node);
	case G_TYPE_DOUBLE: {
		double value = json_node_get_double(node);
		return std::isfinite(value) ? std::llround(value) : 0;
	}
	case G_TYPE_STRING:
		return std::strtoll(json_node_get_string(node), nullptr, 10);
	default:
		return 0;
	}
}

// A member as a real number, from an int, a float or text with an
// optional percent sign — the utilisation figures arrive in all three.
std::optional<double> number_member(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return std::nullopt;
	}
	JsonNode *node = json_object_get_member(object, name);
	if (node == nullptr || json_node_get_node_type(node) != JSON_NODE_VALUE) {
		return std::nullopt;
	}
	switch (json_node_get_value_type(node)) {
	case G_TYPE_INT64:
		return static_cast<double>(json_node_get_int(node));
	case G_TYPE_DOUBLE:
		return json_node_get_double(node);
	case G_TYPE_STRING: {
		const char *text = json_node_get_string(node);
		if (text == nullptr) {
			return std::nullopt;
		}
		std::string digits{text};
		if (!digits.empty() && digits.back() == '%') {
			digits.pop_back();
		}
		char *end = nullptr;
		double value = std::strtod(digits.c_str(), &end);
		if (end == digits.c_str()) {
			return std::nullopt;
		}
		return value;
	}
	default:
		return std::nullopt;
	}
}


// A token count under either key style the tools have used.
long long usage_token(JsonObject *usage, const char *first, const char *second) {
	long long value = int_member(usage, first);
	return value != 0 ? value : int_member(usage, second);
}

// ------------------------------------------------------------- local days

// Days since 1970-01-01 of a civil date, and the inverse (Howard Hinnant's
// algorithms): the calendar math the day buckets and labels need, without
// walking a time-zone database by hand.
long long days_from_civil(long long y, int m, int d) {
	y -= m <= 2;
	const long long era = (y >= 0 ? y : y - 399) / 400;
	const auto yoe = static_cast<long long>(y - era * 400);
	const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

void civil_from_days(long long z, int &y, int &m, int &d) {
	z += 719468;
	const long long era = (z >= 0 ? z : z - 146096) / 146097;
	const auto doe = static_cast<long long>(z - era * 146097);
	const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const long long y_ = era * 400 + yoe;
	const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const long long mp = (5 * doy + 2) / 153;
	d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
	m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
	y = static_cast<int>(y_ + (m <= 2));
}

std::string date_string(int y, int m, int d) {
	return std::format("{:04}-{:02}-{:02}", y, m, d);
}

std::string local_day_of(time_t epoch) {
	tm parts{};
	if (localtime_r(&epoch, &parts) == nullptr) {
		return {};
	}
	return date_string(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
}

std::string today_string() {
	return local_day_of(time(nullptr));
}

// The last seven local dates, oldest first, today last.
std::vector<std::string> recent_dates() {
	tm parts{};
	time_t now = time(nullptr);
	if (localtime_r(&now, &parts) == nullptr) {
		return {};
	}
	long long days = days_from_civil(parts.tm_year + 1900, parts.tm_mon + 1,
		parts.tm_mday);
	std::vector<std::string> dates;
	for (int offset = 6; offset >= 0; offset--) {
		int y = 0, m = 0, d = 0;
		civil_from_days(days - offset, y, m, d);
		dates.push_back(date_string(y, m, d));
	}
	return dates;
}

bool all_digits(const std::string &text) {
	return !text.empty()
		&& text.find_first_not_of("0123456789") == std::string::npos;
}

// The local date of a timestamp value: an epoch in seconds or
// milliseconds, or ISO-8601. An ISO time with an offset names the instant
// and lands on its local date; one without names a local wall-clock time
// and lands on the date it spells. Unparseable or empty means today, the
// same fallback the tools' own records use.
std::string local_day_of_value(const std::string &raw) {
	std::string text = raw;
	while (!text.empty() && (text.back() == ' ' || text.back() == '\n'
			|| text.back() == '\r')) {
		text.pop_back();
	}
	if (text.empty()) {
		return today_string();
	}
	if (all_digits(text)) {
		long long epoch = std::strtoll(text.c_str(), nullptr, 10);
		if (epoch > 10'000'000'000) {
			epoch /= 1000;
		}
		return local_day_of(static_cast<time_t>(epoch));
	}
	int y = 0, m = 0, d = 0, hour = 0, minute = 0, second = 0;
	int scanned = std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d,
		&hour, &minute, &second);
	if (scanned < 3 || y < 1 || m < 1 || m > 12 || d < 1 || d > 31) {
		return today_string();
	}
	if (scanned == 3) { // a bare date
		return date_string(y, m, d);
	}
	// The zone, if any, starts right after the time (fractional seconds
	// included): 'Z' or 'z' names UTC, +hh:mm an offset, and nothing at
	// all a local wall clock.
	std::size_t zone = text.find_first_not_of("0123456789.:", 19);
	if (zone != std::string::npos && (text[zone] == 'Z' || text[zone] == 'z')) {
		long long epoch = days_from_civil(y, m, d) * 86400 + hour * 3600
			+ minute * 60 + second;
		return local_day_of(static_cast<time_t>(epoch));
	}
	if (zone != std::string::npos
			&& (text[zone] == '+' || text[zone] == '-')) {
		int oh = 0, om = 0;
		char sign = 0;
		if (std::sscanf(text.c_str() + zone, "%c%d:%d", &sign, &oh, &om) == 3
				&& (sign == '+' || sign == '-')) {
			long long offset = (oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
			long long epoch = days_from_civil(y, m, d) * 86400 + hour * 3600
				+ minute * 60 + second - offset;
			return local_day_of(static_cast<time_t>(epoch));
		}
	}
	return date_string(y, m, d); // a local wall-clock time
}

// The weekday label of a date, "Sat" … "Fri"; the panel's own "Today"
// rides on the record's flag, not the label.
std::string weekday_label(const std::string &date) {
	int y = 0, m = 0, d = 0;
	if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
		return date;
	}
	static constexpr const char *names[] = {"Sun", "Mon", "Tue", "Wed", "Thu",
		"Fri", "Sat"};
	// 1970-01-01 was a Thursday.
	long long days = days_from_civil(y, m, d);
	return names[static_cast<std::size_t>((days + 4) % 7 + 7) % 7];
}

// A member that holds a timestamp, under either of two owners.
std::string timestamp_member(JsonObject *outer, JsonObject *inner,
	const char *name) {
	std::string raw = str_member(inner, name);
	return raw.empty() ? str_member(outer, name) : raw;
}

std::string iso_utc(time_t epoch) {
	tm parts{};
	if (gmtime_r(&epoch, &parts) == nullptr) {
		return {};
	}
	return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
		parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
		parts.tm_min, parts.tm_sec);
}

// A reset time as ISO-8601: epoch seconds (or milliseconds) become UTC;
// anything already ISO-shaped travels as it is.
std::string normalize_reset_at(const std::string &raw) {
	if (raw.empty()) {
		return {};
	}
	if (all_digits(raw)) {
		long long epoch = std::strtoll(raw.c_str(), nullptr, 10);
		if (epoch == 0) {
			return {};
		}
		if (epoch >= 1'000'000'000'000) {
			epoch /= 1000;
		}
		return iso_utc(static_cast<time_t>(epoch));
	}
	return raw;
}

// The instant an ISO-8601 string names, for telling a window that has
// reset from one still open; nullopt when it will not parse.
std::optional<time_t> epoch_of_iso(const std::string &raw) {
	if (raw.empty()) {
		return std::nullopt;
	}
	int y = 0, m = 0, d = 0, hour = 0, minute = 0, second = 0;
	int scanned = std::sscanf(raw.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d,
		&hour, &minute, &second);
	if (scanned < 3) {
		return std::nullopt;
	}
	if (scanned == 3) {
		hour = minute = second = 0;
	}
	long long offset = 0;
	std::size_t zone = raw.find_first_not_of("0123456789.:", 10);
	char zone_mark = zone != std::string::npos ? raw[zone] : '\0';
	int oh = 0, om = 0;
	char sign = 0;
	if (zone_mark != '\0' && zone_mark != 'Z' && zone_mark != 'z'
			&& std::sscanf(raw.c_str() + zone, "%c%d:%d", &sign, &oh, &om) == 3
			&& (sign == '+' || sign == '-')) {
		offset = (oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
	}
	return static_cast<time_t>(days_from_civil(y, m, d) * 86400 + hour * 3600
		+ minute * 60 + second - offset);
}

// --------------------------------------------------------------- scanning

// One counted message, as the cache keeps it and the merge walks it.
struct Counted {
	std::string key;    // the dedup key, global across every source
	std::string day;    // local YYYY-MM-DD
	std::string family; // the family key
	std::string model;  // the model id
	std::string session;
	Usage usage;
};

struct FileEntry {
	std::string mtime; // "<sec>.<nanos>", the stat that validated it
	long long size = 0;
	std::vector<Counted> messages;
};

struct FamilyStats {
	std::map<std::string, long long> day_tokens;
	std::map<std::string, Usage> model_usage;
	long long prompts = 0;
	std::set<std::string> sessions;
};

std::string stat_signature(const std::string &path) {
	struct stat st{};
	if (stat(path.c_str(), &st) != 0) {
		return {};
	}
	return std::format("{}.{}", st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
}

long long stat_size(const std::string &path) {
	struct stat st{};
	return stat(path.c_str(), &st) == 0 ? st.st_size : -1;
}

time_t stat_mtime_epoch(const std::string &path) {
	struct stat st{};
	return stat(path.c_str(), &st) == 0 ? st.st_mtim.tv_sec : time(nullptr);
}
// The model id as one row's key: providers qualify their ids with the
// routing prefix (`openai/gpt-5.5`, `fireworks:…/glm-5p3-fast`), and the
// same model must not split into two rows because two tools write it two
// ways.
std::string model_key(const std::string &model_id) {
	std::size_t slash = model_id.rfind('/');
	if (slash != std::string::npos && slash + 1 < model_id.size()) {
		return model_id.substr(slash + 1);
	}
	return model_id;
}

// One assistant message out of a Claude Code transcript line, or none.
// The usage rides under `message.usage` (or the entry's own `usage`) in
// either key style, and a message id is the dedup key: the same message id
// in two files counts once.
std::optional<Counted> transcript_message(const std::string &line,
	const std::string &path, int line_no) {
	if (line.find("\"usage\"") == std::string::npos) {
		return std::nullopt;
	}
	ParserPtr parser = parse_json(line);
	JsonObject *entry = root_object(parser);
	if (entry == nullptr) {
		return std::nullopt;
	}
	JsonObject *message = obj_member(entry, "message");
	const bool assistant = str_member(entry, "type") == "assistant"
		|| (message != nullptr && str_member(message, "role") == "assistant");
	if (!assistant) {
		return std::nullopt;
	}
	JsonObject *usage = obj_member(message, "usage");
	if (usage == nullptr) {
		usage = obj_member(entry, "usage");
	}
	if (usage == nullptr) {
		return std::nullopt;
	}
	Usage tokens{
		.input = usage_token(usage, "input_tokens", "inputTokens"),
		.output = usage_token(usage, "output_tokens", "outputTokens"),
		.cache_read = usage_token(usage, "cache_read_input_tokens",
			"cacheReadInputTokens"),
		.cache_write = usage_token(usage, "cache_creation_input_tokens",
			"cacheCreationInputTokens"),
	};
	if (tokens.total() <= 0) {
		return std::nullopt;
	}
	std::string id = str_member(message, "id");
	if (id.empty()) {
		id = str_member(entry, "messageId");
	}
	std::string key = !id.empty() ? id : path + ":"
		+ (str_member(entry, "uuid").empty() ? str_member(entry, "requestId")
						     : str_member(entry, "uuid"))
		+ ":" + std::to_string(line_no);
	std::string model = model_key(str_member(message, "model"));
	if (model.empty()) {
		model = model_key(str_member(entry, "model"));
	}
	if (model.empty()) {
		model = "claude";
	}
	std::string session = str_member(entry, "sessionId");
	if (session.empty()) {
		session = path;
	}
	return Counted{
		.key = std::move(key),
		.day = local_day_of_value(timestamp_member(entry, message, "timestamp")),
		.family = "claude",
		.model = std::move(model),
		.session = std::move(session),
		.usage = tokens,
	};
}

// One assistant message out of a pi/omp session line, or none. Only the
// message's own usage counts; the entry id plus the file place it, so a
// resumed session's rewritten lines count once.
std::optional<Counted> agent_message(const std::string &line,
	const std::string &path, int line_no) {
	if (line.find("\"usage\"") == std::string::npos
			|| line.find("\"assistant\"") == std::string::npos) {
		return std::nullopt;
	}
	ParserPtr parser = parse_json(line);
	JsonObject *entry = root_object(parser);
	if (entry == nullptr || str_member(entry, "type") != "message") {
		return std::nullopt;
	}
	JsonObject *message = obj_member(entry, "message");
	if (message == nullptr || str_member(message, "role") != "assistant") {
		return std::nullopt;
	}
	JsonObject *usage = obj_member(message, "usage");
	if (usage == nullptr) {
		return std::nullopt;
	}
	Usage tokens{
		.input = usage_token(usage, "input", "inputTokens"),
		.output = usage_token(usage, "output", "outputTokens"),
		.cache_read = usage_token(usage, "cacheRead", "cache_read_input_tokens"),
		.cache_write = usage_token(usage, "cacheWrite",
			"cache_creation_input_tokens"),
	};
	if (tokens.total() <= 0) {
		// Some entries carry only the grand total; it all reads as input.
		long long total = int_member(usage, "totalTokens");
		if (total <= 0) {
			return std::nullopt;
		}
		tokens.input = total;
	}
	std::string model = model_key(str_member(message, "model"));
	if (model.empty()) {
		model = "unknown";
	}
	std::string id = str_member(entry, "id");
	return Counted{
		.key = path + ":" + (id.empty() ? std::to_string(line_no) : id),
		.day = local_day_of_value(timestamp_member(entry, message, "timestamp")),
		.family = family_of(str_member(message, "provider"),
			str_member(message, "api"), model),
		.model = std::move(model),
		.session = path,
		.usage = tokens,
	};
}

// The counted token_count entries of one Codex session file. The model
// rides on turn_context lines; last_token_usage is one turn's own usage,
// with the cached tokens already inside input_tokens.
FileEntry scan_codex_file(const std::string &path) {
	FileEntry entry;
	entry.mtime = stat_signature(path);
	entry.size = stat_size(path);
	std::string current_model = "codex";
	std::ifstream file(path);
	std::string line;
	for (int line_no = 1; std::getline(file, line); line_no++) {
		ParserPtr parser = parse_json(line);
		JsonObject *root = root_object(parser);
		if (root == nullptr) {
			continue;
		}
		JsonObject *payload = obj_member(root, "payload");
		if (payload == nullptr) {
			payload = root;
		}
		if (str_member(root, "type") == "turn_context") {
			std::string model = str_member(payload, "model");
			if (model.empty()) {
				model = str_member(payload, "model_slug");
			}
			if (!model.empty()) {
				current_model = model_key(model);
			}
			continue;
		}
		if (str_member(root, "type") == "response_item") {
			JsonObject *inner = obj_member(payload, "payload");
			if (inner != nullptr) {
				payload = inner;
			}
		}
		if (str_member(payload, "type") != "token_count") {
			continue;
		}
		JsonObject *info = obj_member(payload, "info");
		JsonObject *usage = info != nullptr
			? obj_member(info, "last_token_usage") : nullptr;
		if (usage == nullptr) {
			continue;
		}
		long long cache_read = int_member(usage, "cached_input_tokens");
		long long cache_write = int_member(usage, "cache_write_input_tokens");
		Usage tokens{
			.input = std::max(0LL,
				int_member(usage, "input_tokens") - cache_read - cache_write),
			.output = int_member(usage, "output_tokens"),
			.cache_read = cache_read,
			.cache_write = cache_write,
		};
		if (tokens.total() <= 0) {
			continue;
		}
		std::string day = str_member(root, "timestamp");
		entry.messages.push_back(Counted{
			.key = path + ":" + std::to_string(line_no),
			.day = local_day_of_value(day.empty()
				? std::to_string(stat_mtime_epoch(path)) : day),
			.family = "gpt",
			.model = current_model,
			.session = path,
			.usage = tokens,
		});
	}
	return entry;
}

FileEntry scan_jsonl(const std::string &path,
	const std::function<std::optional<Counted>(const std::string &,
		const std::string &, int)> &parse) {
	FileEntry entry;
	entry.mtime = stat_signature(path);
	entry.size = stat_size(path);
	std::ifstream file(path);
	std::string line;
	for (int line_no = 1; std::getline(file, line); line_no++) {
		if (std::optional<Counted> counted = parse(line, path, line_no)) {
			entry.messages.push_back(std::move(*counted));
		}
	}
	return entry;
}

// The opencode database's assistant messages, one row each. Read-only and
// tolerant: opencode may be writing, and one malformed row skips itself.
FileEntry scan_opencode_db(const std::string &db) {
	FileEntry entry;
	entry.mtime = stat_signature(db);
	entry.size = stat_size(db);
	std::string uri = std::filesystem::path(db).string();
	// The URI form wants a scheme and percent-safe path; open the plain
	// path with the immutable flag instead, which is read-only too.
	sqlite3 *handle = nullptr;
	if (sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr)
			!= SQLITE_OK) {
		if (handle != nullptr) {
			sqlite3_close(handle);
		}
		return entry;
	}
	sqlite3_stmt *statement = nullptr;
	if (sqlite3_prepare_v2(handle, "SELECT session_id, data FROM message", -1,
			    &statement, nullptr) == SQLITE_OK) {
		long long row = 0;
		while (sqlite3_step(statement) == SQLITE_ROW) {
			row++;
			const char *session_text = reinterpret_cast<const char *>(
				sqlite3_column_text(statement, 0));
			const char *raw = reinterpret_cast<const char *>(
				sqlite3_column_text(statement, 1));
			if (session_text == nullptr || raw == nullptr) {
				continue;
			}
			std::string session = session_text;
			ParserPtr parser = parse_json(raw);
			JsonObject *message = root_object(parser);
			if (message == nullptr
					|| str_member(message, "role") != "assistant") {
				continue;
			}
			JsonObject *tokens = obj_member(message, "tokens");
			JsonObject *cache = tokens != nullptr
				? obj_member(tokens, "cache") : nullptr;
			Usage usage{
				.input = tokens != nullptr
					? int_member(tokens, "input") : 0,
				// opencode keeps thinking tokens out of output; both are
				// generated.
				.output = tokens != nullptr
					? int_member(tokens, "output")
						+ int_member(tokens, "reasoning") : 0,
				.cache_read = cache != nullptr
					? int_member(cache, "read") : 0,
				.cache_write = cache != nullptr
					? int_member(cache, "write") : 0,
			};
			if (usage.total() <= 0) {
				continue;
			}
			std::string model = str_member(message, "modelID");
			std::size_t slash = model.rfind('/');
			if (slash != std::string::npos && slash + 1 < model.size()) {
				model = model.substr(slash + 1);
			}
			if (model.empty()) {
				model = "unknown";
			}
			JsonObject *time = obj_member(message, "time");
			entry.messages.push_back(Counted{
				.key = "opencode:" + session + ":" + std::to_string(row),
				.day = local_day_of_value(time != nullptr
					? str_member(time, "created") : ""),
				.family = family_of(str_member(message, "providerID"), "",
					model),
				.model = std::move(model),
				.session = "opencode:" + session,
				.usage = usage,
			});
		}
	}
	if (statement != nullptr) {
		sqlite3_finalize(statement);
	}
	sqlite3_close(handle);
	return entry;
}

// Every *.jsonl under a root, sorted, so the merge order is stable.
std::vector<std::string> jsonl_files(const std::string &root) {
	std::vector<std::string> files;
	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec)) {
		return files;
	}
	for (const auto &item : std::filesystem::recursive_directory_iterator(root,
			std::filesystem::directory_options::skip_permission_denied, ec)) {
		if (ec || !item.is_regular_file(ec)) {
			continue;
		}
		if (item.path().extension() == ".jsonl") {
			files.push_back(item.path().string());
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

// ---------------------------------------------------------------- caching

constexpr int cache_version = 1;

Counted counted_from_json(JsonObject *object) {
	return Counted{
		.key = str_member(object, "k"),
		.day = str_member(object, "d"),
		.family = str_member(object, "f"),
		.model = str_member(object, "m"),
		.session = str_member(object, "s"),
		.usage = Usage{
			.input = int_member(object, "i"),
			.output = int_member(object, "o"),
			.cache_read = int_member(object, "r"),
			.cache_write = int_member(object, "w"),
		},
	};
}

// One counted message as a cache entry, inside the array being built.


void cache_message(JsonBuilder *builder, const Counted &counted) {
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "k");
	json_builder_add_string_value(builder, counted.key.c_str());
	json_builder_set_member_name(builder, "d");
	json_builder_add_string_value(builder, counted.day.c_str());
	json_builder_set_member_name(builder, "f");
	json_builder_add_string_value(builder, counted.family.c_str());
	json_builder_set_member_name(builder, "m");
	json_builder_add_string_value(builder, counted.model.c_str());
	json_builder_set_member_name(builder, "s");
	json_builder_add_string_value(builder, counted.session.c_str());
	json_builder_set_member_name(builder, "i");
	json_builder_add_int_value(builder, counted.usage.input);
	json_builder_set_member_name(builder, "o");
	json_builder_add_int_value(builder, counted.usage.output);
	json_builder_set_member_name(builder, "r");
	json_builder_add_int_value(builder, counted.usage.cache_read);
	json_builder_set_member_name(builder, "w");
	json_builder_add_int_value(builder, counted.usage.cache_write);
	json_builder_end_object(builder);
}

struct CacheLoadContext {
	std::map<std::string, FileEntry> *files = nullptr;
};

void cache_load_file(JsonObject *, const char *name, JsonNode *node,
	void *user) {
	auto *context = static_cast<CacheLoadContext *>(user);
	FileEntry entry;
	JsonObject *object = json_node_get_node_type(node) == JSON_NODE_OBJECT
		? json_node_get_object(node) : nullptr;
	if (object == nullptr) {
		return;
	}
	entry.mtime = str_member(object, "mtime");
	entry.size = int_member(object, "size");
	JsonNode *messages_node = json_object_has_member(object, "messages")
		? json_object_get_member(object, "messages") : nullptr;
	if (messages_node == nullptr
			|| json_node_get_node_type(messages_node) != JSON_NODE_ARRAY) {
		(*context->files)[name] = std::move(entry);
		return;
	}
	JsonArray *messages = json_node_get_array(messages_node);
	for (guint i = 0; i < json_array_get_length(messages); i++) {
		JsonNode *message = json_array_get_element(messages, i);
		if (json_node_get_node_type(message) == JSON_NODE_OBJECT) {
			entry.messages.push_back(counted_from_json(
				json_node_get_object(message)));
		}
	}
	(*context->files)[name] = std::move(entry);
}

std::map<std::string, FileEntry> read_cache(const std::string &path) {
	std::map<std::string, FileEntry> files;
	if (path.empty()) {
		return files;
	}
	std::ifstream file(path);
	if (!file.good()) {
		return files;
	}
	std::string text((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	ParserPtr parser = parse_json(text);
	JsonObject *root = root_object(parser);
	if (root == nullptr || int_member(root, "version") != cache_version) {
		return files; // a cache that is not the shape we write is a miss
	}
	JsonObject *entries = obj_member(root, "files");
	if (entries == nullptr) {
		return files;
	}
	CacheLoadContext context{.files = &files};
	json_object_foreach_member(entries, cache_load_file, &context);
	return files;
}

// Atomic: a writer-unique temp file in the same directory, renamed over
// the target, so a reader never sees a half-written cache.
void write_cache(const std::string &path,
	const std::map<std::string, FileEntry> &files) {
	if (path.empty()) {
		return;
	}
	std::filesystem::path target(path);
	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);
	JsonBuilder *builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, cache_version);
	json_builder_set_member_name(builder, "files");
	json_builder_begin_object(builder);
	for (const auto &[file, entry] : files) {
		json_builder_set_member_name(builder, file.c_str());
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "mtime");
		json_builder_add_string_value(builder, entry.mtime.c_str());
		json_builder_set_member_name(builder, "size");
		json_builder_add_int_value(builder, entry.size);
		json_builder_set_member_name(builder, "messages");
		json_builder_begin_array(builder);
		for (const Counted &counted : entry.messages) {
			cache_message(builder, counted);
		}
		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	JsonNode *root = json_builder_get_root(builder);
	char *text = json_to_string(root, false);
	std::string tmp = (target.parent_path()
		/ ("." + target.filename().string() + "."
			+ std::to_string(getpid()) + ".tmp")).string();
	FILE *f = std::fopen(tmp.c_str(), "w");
	bool written = f != nullptr;
	if (written) {
		std::fwrite(text, 1, std::strlen(text), f);
		std::fclose(f);
	}
	std::free(text);
	json_node_free(root);
	g_object_unref(builder);
	if (written) {
		std::filesystem::rename(tmp, target, ec);
		if (!ec) {
			return;
		}
	}
	std::filesystem::remove(tmp, ec);
}

bool same_cache(const std::map<std::string, FileEntry> &a,
	const std::map<std::string, FileEntry> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (const auto &[path, entry] : a) {
		auto other = b.find(path);
		if (other == b.end() || other->second.mtime != entry.mtime
				|| other->second.size != entry.size
				|| other->second.messages.size() != entry.messages.size()) {
			return false;
		}
		for (std::size_t i = 0; i < entry.messages.size(); i++) {
			if (other->second.messages[i].key != entry.messages[i].key) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

// ---------------------------------------------------------- display names

std::string format_tokens(long long tokens) {
	if (tokens >= 1'000'000'000) {
		return std::format("{:.1f}B", double(tokens) / 1e9);
	}
	if (tokens >= 1'000'000) {
		return std::format("{:.1f}M", double(tokens) / 1e6);
	}
	if (tokens >= 1'000) {
		return std::format("{:.1f}K", double(tokens) / 1e3);
	}
	return std::to_string(tokens);
}

namespace {

std::string word_case(const std::string &word) {
	// The acronyms read as themselves; every other segment is a plain word
	// with a capital.
	static const std::map<std::string, std::string> acronyms = {
		{"gpt", "GPT"}, {"glm", "GLM"}, {"oss", "OSS"}, {"llama", "LLAMA"},
	};
	if (auto acronym = acronyms.find(word); acronym != acronyms.end()) {
		return acronym->second;
	}
	std::string out = word;
	if (!out.empty()) {
		out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
	}
	return out;
}

std::string lower_copy(std::string_view text) {
	std::string out{text};
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return out;
}

} // namespace

std::string model_display_name(std::string_view model_id) {
	std::string name{model_id};
	std::size_t slash = name.rfind('/');
	if (slash != std::string::npos && slash + 1 < name.size()) {
		name = name.substr(slash + 1);
	}
	if (name.starts_with("claude-")) {
		name = name.substr(7);
	}
	if (name.size() > 9 && all_digits(name.substr(name.size() - 8))
			&& name[name.size() - 9] == '-') {
		name = name.substr(0, name.size() - 9); // a dated snapshot suffix
	}
	std::vector<std::string> words;
	std::vector<std::string> version;
	// Joins the numeric run so far into one version word: 5-5 → 5.5.
	auto flush_version = [&words, &version]() {
		if (version.empty()) {
			return;
		}
		std::string joined;
		for (const std::string &piece : version) {
			joined += (joined.empty() ? "" : ".") + piece;
		}
		words.push_back(joined);
		version.clear();
	};
	std::size_t start = 0;
	while (start <= name.size()) {
		std::size_t dash = name.find('-', start);
		std::string part = name.substr(start,
			dash == std::string::npos ? std::string::npos : dash - start);
		if (!part.empty() && std::isdigit(static_cast<unsigned char>(part[0]))) {
			version.push_back(part);
		} else if (!part.empty()) {
			flush_version();
			words.push_back(word_case(part));
		}
		if (dash == std::string::npos) {
			break;
		}
		start = dash + 1;
	}
	flush_version();
	std::string out;
	for (const std::string &word : words) {
		out += (out.empty() ? "" : " ") + word;
	}
	return out.empty() ? "Unknown" : out;
}

std::string family_of(std::string_view provider, std::string_view api,
	std::string_view model_id) {
	std::string label{!provider.empty() ? provider : api};
	if (!label.empty()) {
		if (label == "anthropic") {
			return "claude";
		}
		std::string first = label.substr(0, label.find('-'));
		std::string lowered = lower_copy(first);
		if (lowered == "openai") {
			return "gpt";
		}
		if (lowered == "google" || lowered == "gemini") {
			return "gemini";
		}
		return lowered;
	}
	// No label of its own: the model id names the family.
	std::string model = lower_copy(model_id);
	std::size_t slash = model.rfind('/');
	if (slash != std::string::npos && slash + 1 < model.size()) {
		model = model.substr(slash + 1);
	}
	if (model.starts_with("claude")) {
		return "claude";
	}
	if (model.starts_with("gpt") || model.starts_with("codex")
			|| model.starts_with("o1") || model.starts_with("o3")
			|| model.starts_with("o4")) {
		return "gpt";
	}
	if (model.find("gemini") != std::string::npos) {
		return "gemini";
	}
	return "other";
}

std::string family_chip(std::string_view family) {
	if (family == "claude") {
		return "Claude";
	}
	if (family == "gpt") {
		return "GPT";
	}
	// The gateway families' chips stay short: the pill is narrow, and the
	// hero right above the rows says the full name.
	if (family == "anthropic") {
		return "Claude GW";
	}
	return family_title(family);
}
std::string family_title(std::string_view family) {
	if (family == "claude") {
		return "Claude Code";
	}
	if (family == "gpt") {
		return "GPT";
	}
	// An anthropic-* gateway still runs Claude models; the tab says so,
	// and stays separate from the subscription tab whose figures
	// Omarchy's own counter equals.
	if (family == "anthropic") {
		return "Claude (gateway)";
	}
	if (family == "other") {
		return "Other";
	}
	std::string out{family};
	if (!out.empty()) {
		out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
	}
	return out;
}


std::string plan_label(std::string_view tier, std::string_view subscription) {
	std::string lowered = lower_copy(tier);
	std::size_t at = lowered.find("max_");
	if (at != std::string::npos) {
		std::size_t digits = at + 4;
		while (digits < lowered.size()
				&& std::isdigit(static_cast<unsigned char>(lowered[digits]))) {
			digits++;
		}
		if (digits > at + 4 && digits < lowered.size()
				&& lowered[digits] == 'x') {
			return "Max " + std::string{tier.substr(at + 4, digits - at - 4)}
				+ "x";
		}
	}
	if (!subscription.empty()) {
		std::string out{subscription};
		out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
		return out;
	}
	return {};
}

// ----------------------------------------------------------------- limits

std::vector<Limit> parse_anthropic_limits(const std::string &payload) {
	std::vector<Limit> limits;
	ParserPtr parser = parse_json(payload);
	JsonObject *root = root_object(parser);
	if (root == nullptr) {
		return limits;
	}
	JsonObject *session = obj_member(root, "five_hour");
	JsonObject *weekly = obj_member(root, "seven_day_oauth_apps");
	if (weekly == nullptr) {
		weekly = obj_member(root, "seven_day");
	}
	// One payload speaks one convention: if any utilisation value is at or
	// above 1 they are all percentages, and a lone 1.0 is 1%, not 100%.
	bool percent_scale = false;
	for (JsonObject *window : {session, weekly}) {
		if (std::optional<double> value = number_member(window, "utilization")) {
			percent_scale = percent_scale || *value >= 1;
		}
	}
	for (auto [window, title] : {std::pair{session, "Session"},
			std::pair{weekly, "Weekly"}}) {
		if (window == nullptr) {
			continue;
		}
		std::optional<double> value = number_member(window, "utilization");
		if (!value || *value < 0) {
			continue;
		}
		double percent = percent_scale || *value > 1
			? std::min(1.0, *value / 100.0) : std::min(1.0, *value);
		limits.push_back(Limit{
			.title = title,
			.percent = percent,
			.resets_at = normalize_reset_at(str_member(window, "resets_at")),
		});
	}
	return limits;
}

namespace {

Limit codex_window(JsonObject *window, int duration_mins) {
	Limit limit;
	limit.title = duration_mins == 10080 ? "Weekly" : "Session";
	if (std::optional<double> used = number_member(window, "usedPercent")) {
		limit.percent = std::min(1.0, std::max(0.0, *used / 100.0));
	}
	limit.resets_at = normalize_reset_at(str_member(window, "resetsAt"));
	return limit;
}

} // namespace

CodexLimits parse_codex_limits(const std::string &account_result,
	const std::string &rate_limits_result) {
	CodexLimits out;
	ParserPtr limits_parser = parse_json(rate_limits_result);
	JsonObject *limits_root = root_object(limits_parser);
	JsonObject *rate_limits = obj_member(
		obj_member(limits_root, "result"), "rateLimits");
	ParserPtr account_parser = parse_json(account_result);
	JsonObject *account = obj_member(
		obj_member(root_object(account_parser), "result"), "account");
	if (rate_limits != nullptr) {
		out.plan = str_member(rate_limits, "planType");
		if (out.plan.empty() && account != nullptr) {
			out.plan = str_member(account, "planType");
		}
		if (out.plan.empty() && account != nullptr) {
			out.plan = str_member(account, "type");
		}
		for (const char *window : {"primary", "secondary"}) {
			if (JsonObject *entry = obj_member(rate_limits, window)) {
				Limit limit = codex_window(entry,
					int_member(entry, "windowDurationMins"));
				if (limit.percent >= 0) {
					out.limits.push_back(std::move(limit));
				}
			}
		}
	}
	return out;
}

// ------------------------------------------------------------- collection

Record collect(const Paths &paths,
	const std::map<std::string, ProviderLimits> &limits) {
	Record record;
	record.updated_at = iso_utc(time(nullptr));
	std::map<std::string, FileEntry> cached = read_cache(paths.cache_file);
	std::map<std::string, FileEntry> fresh;
	std::set<std::string> seen;
	std::map<std::string, FamilyStats> families;

	// A source file and how it is parsed. Codex session files older than
	// thirty days are not read, the same window the CLI keeps hot.
	struct Source {
		std::string path;
		bool codex = false;
	};
	std::vector<Source> sources;
	auto add = [&](const std::string &root, bool codex, bool age_cutoff) {
		for (const std::string &path : jsonl_files(root)) {
			if (age_cutoff && stat_mtime_epoch(path) < time(nullptr) - 30 * 86400) {
				continue;
			}
			sources.push_back({.path = path, .codex = codex});
		}
	};
	add(paths.claude_dir + "/projects", false, false);
	add(paths.pi_sessions, false, false);
	add(paths.omp_sessions, false, false);
	add(paths.codex_home + "/sessions", true, true);
	add(paths.codex_home + "/archived_sessions", true, true);
	std::error_code ec;
	if (!paths.opencode_db.empty()
			&& std::filesystem::is_regular_file(paths.opencode_db, ec)) {
		sources.push_back({.path = paths.opencode_db});
	}

	for (const Source &source : sources) {
		FileEntry entry;
		std::string signature = stat_signature(source.path);
		long long size = stat_size(source.path);
		if (auto hit = cached.find(source.path); hit != cached.end()
				&& hit->second.mtime == signature && hit->second.size == size) {
			entry = hit->second; // unchanged since the last scan
		} else if (source.path == paths.opencode_db) {
			entry = scan_opencode_db(source.path);
		} else if (source.codex) {
			entry = scan_codex_file(source.path);
		} else {
			// Claude transcripts and pi/omp sessions are both jsonl of
			// assistant messages; they differ in where the provider
			// lives, which decides the family.
			bool transcript = source.path.rfind(paths.claude_dir + "/projects",
				0) == 0;
			entry = scan_jsonl(source.path, transcript
				? transcript_message : agent_message);
		}
		for (const Counted &counted : entry.messages) {
			if (!seen.insert(counted.key).second) {
				continue;
			}
			FamilyStats &stats = families[counted.family];
			stats.day_tokens[counted.day] += counted.usage.total();
			Usage &bucket = stats.model_usage[counted.model];
			bucket.input += counted.usage.input;
			bucket.output += counted.usage.output;
			bucket.cache_read += counted.usage.cache_read;
			bucket.cache_write += counted.usage.cache_write;
			stats.prompts++;
			stats.sessions.insert(counted.session);
		}
		fresh[source.path] = std::move(entry);
	}
	if (!same_cache(cached, fresh)) {
		write_cache(paths.cache_file, fresh);
	}

	// A provider earns its place by having something to show: usage of its
	// own, or limits a signed-in CLI reported.
	auto visible = [&](const std::string &family) {
		auto stats = families.find(family);
		auto lim = limits.find(family);
		return (stats != families.end()
			     && (stats->second.prompts > 0 || !stats->second.sessions.empty()))
			|| (lim != limits.end() && !lim->second.limits.empty());
	};
	std::vector<std::string> order;
	for (const char *first : {"claude", "gpt"}) {
		if (visible(first)) {
			order.push_back(first);
		}
	}
	std::vector<std::pair<long long, std::string>> rest;
	for (const auto &[family, stats] : families) {
		if (family == "claude" || family == "gpt" || !visible(family)) {
			continue;
		}
		long long week = 0;
		for (const std::string &date : recent_dates()) {
			if (auto day = stats.day_tokens.find(date);
					day != stats.day_tokens.end()) {
				week += day->second;
			}
		}
		rest.push_back({-week, family});
	}
	std::sort(rest.begin(), rest.end());
	for (const auto &entry : rest) {
		order.push_back(entry.second);
	}

	std::string today = today_string();
	std::vector<std::string> dates = recent_dates();
	for (const std::string &family : order) {
		ProviderRecord provider;
		provider.id = family;
		provider.chip = family_chip(family);
		provider.title = family_title(family);
		if (auto lim = limits.find(family); lim != limits.end()) {
			provider.limits = lim->second.limits;
			provider.tier = lim->second.tier;
			provider.status = lim->second.status;
			provider.help = lim->second.help;
		}
		for (const std::string &date : dates) {
			long long tokens = 0;
			if (auto stats = families.find(family); stats != families.end()) {
				if (auto day = stats->second.day_tokens.find(date);
						day != stats->second.day_tokens.end()) {
					tokens = day->second;
				}
			}
			provider.days.push_back(DayTokens{
				.date = date,
				.label = date == today ? "Today" : weekday_label(date),
				.today = date == today,
				.tokens = tokens,
				.tokens_text = format_tokens(tokens),
			});
		}
		if (auto stats = families.find(family); stats != families.end()) {
			for (const auto &[model, usage] : stats->second.model_usage) {
				provider.models.push_back(ModelUsage{
					.id = model,
					.name = model_display_name(model),
					.usage = usage,
					.total_text = format_tokens(usage.total()),
				});
			}
			std::sort(provider.models.begin(), provider.models.end(),
				[](const ModelUsage &a, const ModelUsage &b) {
					return a.total() != b.total() ? a.total() > b.total()
						: a.id < b.id;
				});
			provider.total_prompts = stats->second.prompts;
			provider.total_sessions = static_cast<long long>(
				stats->second.sessions.size());
		}
		record.providers.push_back(std::move(provider));
	}
	return record;
}

std::string record_json(const Record &record) {
	JsonBuilder *builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "updatedAt");
	json_builder_add_string_value(builder, record.updated_at.c_str());
	json_builder_set_member_name(builder, "providers");
	json_builder_begin_array(builder);
	for (const ProviderRecord &provider : record.providers) {
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, provider.id.c_str());
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, provider.chip.c_str());
		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder, provider.title.c_str());
		json_builder_set_member_name(builder, "tierLabel");
		json_builder_add_string_value(builder, provider.tier.c_str());
		json_builder_set_member_name(builder, "usageStatusText");
		json_builder_add_string_value(builder, provider.status.c_str());
		json_builder_set_member_name(builder, "authHelpText");
		json_builder_add_string_value(builder, provider.help.c_str());
		json_builder_set_member_name(builder, "limits");
		json_builder_begin_array(builder);
		for (const Limit &limit : provider.limits) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "title");
			json_builder_add_string_value(builder, limit.title.c_str());
			json_builder_set_member_name(builder, "percent");
			json_builder_add_double_value(builder, limit.percent);
			json_builder_set_member_name(builder, "resetsAt");
			json_builder_add_string_value(builder, limit.resets_at.c_str());
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
		json_builder_set_member_name(builder, "days");
		json_builder_begin_array(builder);
		for (const DayTokens &day : provider.days) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "date");
			json_builder_add_string_value(builder, day.date.c_str());
			json_builder_set_member_name(builder, "label");
			json_builder_add_string_value(builder, day.label.c_str());
			json_builder_set_member_name(builder, "today");
			json_builder_add_boolean_value(builder, day.today);
			json_builder_set_member_name(builder, "tokens");
			json_builder_add_int_value(builder, day.tokens);
			json_builder_set_member_name(builder, "tokensText");
			json_builder_add_string_value(builder, day.tokens_text.c_str());
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
		json_builder_set_member_name(builder, "models");
		json_builder_begin_array(builder);
		for (const ModelUsage &model : provider.models) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "id");
			json_builder_add_string_value(builder, model.id.c_str());
			json_builder_set_member_name(builder, "name");
			json_builder_add_string_value(builder, model.name.c_str());
			json_builder_set_member_name(builder, "total");
			json_builder_add_int_value(builder, model.total());
			json_builder_set_member_name(builder, "totalText");
			json_builder_add_string_value(builder, model.total_text.c_str());
			json_builder_set_member_name(builder, "input");
			json_builder_add_int_value(builder, model.usage.input);
			json_builder_set_member_name(builder, "output");
			json_builder_add_int_value(builder, model.usage.output);
			json_builder_set_member_name(builder, "cacheRead");
			json_builder_add_int_value(builder, model.usage.cache_read);
			json_builder_set_member_name(builder, "cacheWrite");
			json_builder_add_int_value(builder, model.usage.cache_write);
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
		json_builder_set_member_name(builder, "totalPrompts");
		json_builder_add_int_value(builder, provider.total_prompts);
		json_builder_set_member_name(builder, "totalSessions");
		json_builder_add_int_value(builder, provider.total_sessions);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	JsonNode *root = json_builder_get_root(builder);
	char *text = json_to_string(root, false);
	std::string out{text != nullptr ? text : ""};
	std::free(text);
	json_node_free(root);
	g_object_unref(builder);
	return out;
}

// ------------------------------------------------------------ probe cache

namespace {

void limit_to_json(JsonBuilder *builder, const Limit &limit) {
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, limit.title.c_str());
	json_builder_set_member_name(builder, "percent");
	json_builder_add_double_value(builder, limit.percent);
	json_builder_set_member_name(builder, "resetsAt");
	json_builder_add_string_value(builder, limit.resets_at.c_str());
	json_builder_end_object(builder);
}

Limit limit_from_json(JsonObject *object) {
	Limit limit;
	limit.title = str_member(object, "title");
	if (std::optional<double> percent = number_member(object, "percent")) {
		limit.percent = *percent;
	}
	limit.resets_at = str_member(object, "resetsAt");
	return limit;
}

} // namespace

std::optional<LimitsCache> read_limits_cache(const std::string &path) {
	if (path.empty()) {
		return std::nullopt;
	}
	std::ifstream file(path);
	if (!file.good()) {
		return std::nullopt;
	}
	std::string text((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	ParserPtr parser = parse_json(text);
	JsonObject *root = root_object(parser);
	LimitsCache cache;
	cache.fetched_at_ms = int_member(root, "fetchedAtMs");
	JsonNode *limits_node = root != nullptr
		&& json_object_has_member(root, "limits")
			? json_object_get_member(root, "limits") : nullptr;
	if (limits_node == nullptr
			|| json_node_get_node_type(limits_node) != JSON_NODE_ARRAY) {
		return std::nullopt;
	}
	JsonArray *limits = json_node_get_array(limits_node);
	for (guint i = 0; i < json_array_get_length(limits); i++) {
		JsonNode *entry = json_array_get_element(limits, i);
		if (json_node_get_node_type(entry) == JSON_NODE_OBJECT) {
			cache.limits.push_back(limit_from_json(json_node_get_object(entry)));
		}
	}
	return cache;
}

void write_limits_cache(const std::string &path, const LimitsCache &cache) {
	if (path.empty()) {
		return;
	}
	std::filesystem::path target(path);
	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);
	JsonBuilder *builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "fetchedAtMs");
	json_builder_add_int_value(builder, cache.fetched_at_ms);
	json_builder_set_member_name(builder, "limits");
	json_builder_begin_array(builder);
	for (const Limit &limit : cache.limits) {
		limit_to_json(builder, limit);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	JsonNode *root = json_builder_get_root(builder);
	char *text = json_to_string(root, false);
	std::string tmp = (target.parent_path()
		/ ("." + target.filename().string() + "."
			+ std::to_string(getpid()) + ".tmp")).string();
	FILE *f = std::fopen(tmp.c_str(), "w");
	bool written = f != nullptr;
	if (written) {
		std::fwrite(text, 1, std::strlen(text), f);
		std::fclose(f);
	}
	std::free(text);
	json_node_free(root);
	g_object_unref(builder);
	if (written) {
		std::filesystem::rename(tmp, target, ec);
		if (!ec) {
			return;
		}
	}
	std::filesystem::remove(tmp, ec);
}

std::vector<Limit> open_limits(const std::vector<Limit> &limits) {
	std::vector<Limit> open;
	time_t now = time(nullptr);
	for (const Limit &limit : limits) {
		std::optional<time_t> reset = epoch_of_iso(limit.resets_at);
		if (!reset || *reset > now) {
			open.push_back(limit);
		}
	}
	return open;
}

} // namespace agent_usage
