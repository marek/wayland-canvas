#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "desktop_entry.h"

namespace {

constexpr char ascii_lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c;
}

// Trims leading/trailing spaces and tabs.
constexpr std::string_view trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.remove_suffix(1);
	return s;
}

constexpr bool bool_val(std::string_view v) {
	constexpr std::string_view true_ = "true";
	if (v.size() != true_.size())
		return false;
	for (size_t i = 0; i < v.size(); i++)
		if (ascii_lower(v[i]) != true_[i])
			return false;
	return true;
}

constexpr bool ci_starts_with(std::string_view s, std::string_view q) {
	if (s.size() < q.size())
		return false;
	for (size_t i = 0; i < q.size(); i++)
		if (ascii_lower(s[i]) != ascii_lower(q[i]))
			return false;
	return true;
}

constexpr bool ci_contains(std::string_view s, std::string_view q) {
	if (q.empty() || s.size() < q.size())
		return false;
	for (size_t i = 0; i + q.size() <= s.size(); i++)
		if (ci_starts_with(s.substr(i), q))
			return true;
	return false;
}

std::string unescape(std::string_view v) {
	std::string out;
	out.reserve(v.size());
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] != '\\') {
			out += v[i];
			continue;
		}
		char next = i + 1 < v.size() ? v[i + 1] : '\0';
		switch (next) {
		case 's': out += ' ';  i++; break;
		case 'n': out += '\n'; i++; break;
		case 't': out += '\t'; i++; break;
		case 'r': out += '\r'; i++; break;
		case '\\': out += '\\'; i++; break;
		default: out += '\\'; break; // unknown escape: keep backslash
		}
	}
	return out;
}

// ---- key/value storage for one file ----

using Kvs = std::vector<std::pair<std::string, std::string>>; // key "Name" or "Name[de_DE]"

const std::string *kv_get(const Kvs &kvs, std::string_view key) {
	for (const auto &kv : kvs)
		if (std::string_view(kv.first) == key)
			return &kv.second;
	return nullptr;
}

bool bool_kv(const Kvs &kvs, std::string_view key) {
	const std::string *v = kv_get(kvs, key);
	return v && bool_val(*v);
}

/*
 * Locale keys in spec order for `lang` (e.g. "de_DE.UTF-8@euro"):
 * de_DE@euro, de_DE, de@euro, de. ""/"C"/"POSIX" localize nothing.
 * The lang string is truncated to 63 bytes first.
 */
std::vector<std::string> locale_variants(std::string_view lang) {
	if (lang.empty() || lang == "C" || lang == "POSIX")
		return {};

	std::string buf{lang.substr(0, 63)};

	if (size_t dot = buf.find('.'); dot != std::string::npos)
		buf.resize(dot); // drop .encoding
	std::string_view mod;
	bool has_mod = false;
	if (size_t at = buf.find('@'); at != std::string::npos) {
		mod = buf;
		mod.remove_prefix(at + 1);
		has_mod = true;
		buf.resize(at);
	}
	std::string_view country;
	bool has_country = false;
	if (size_t us = buf.find('_'); us != std::string::npos) {
		country = buf;
		country.remove_prefix(us + 1);
		has_country = true;
		buf.resize(us);
	}
	if (buf.empty())
		return {};

	std::vector<std::string> out;
	if (has_country && has_mod) {
		std::string v = buf;
		v += '_';
		v += country;
		v += '@';
		v += mod;
		out.push_back(std::move(v));
	}
	if (has_country) {
		std::string v = buf;
		v += '_';
		v += country;
		out.push_back(std::move(v));
	}
	if (has_mod) {
		std::string v = buf;
		v += '@';
		v += mod;
		out.push_back(std::move(v));
	}
	out.push_back(std::move(buf));
	return out;
}

// Best value for `key`: Name[locale] variants in order, then plain Name.
const std::string *kv_localized(const Kvs &kvs, std::string_view key,
	const std::vector<std::string> &lc) {
	for (const std::string &v : lc) {
		std::string localized = std::string(key) + '[' + v + ']';
		if (const std::string *val = kv_get(kvs, localized))
			return val;
	}
	return kv_get(kvs, key);
}

constexpr bool is_field_code(char c) {
	constexpr std::string_view codes = "fFuUdDnNiIckvm";
	return codes.find(c) != std::string_view::npos;
}

} // namespace

std::string clean_exec(std::string_view exec) {
	std::string out;
	out.reserve(exec.size());
	size_t n = exec.size();
	for (size_t i = 0; i < n;) {
		char c = exec[i];
		// a quoted argument that is only a field code: drop the quotes too
		if (c == '"' && i + 3 < n && exec[i + 1] == '%'
				&& is_field_code(exec[i + 2]) && exec[i + 3] == '"') {
			i += 4;
			continue;
		}
		if (c == '%' && i + 1 < n) {
			if (exec[i + 1] == '%') {
				out += '%';
				i += 2;
				continue;
			}
			if (is_field_code(exec[i + 1])) {
				i += 2;
				continue;
			}
		}
		out += c;
		i++;
	}

	// collapse runs of spaces to one, trim the ends
	std::string w;
	w.reserve(out.size());
	bool last_space = true;
	for (char c : out) {
		if (c == ' ') {
			if (!last_space) {
				w += ' ';
				last_space = true;
			}
		} else {
			w += c;
			last_space = false;
		}
	}
	if (!w.empty() && w.back() == ' ')
		w.pop_back();
	return w;
}

int App::match(std::string_view query) const {
	if (query.empty())
		return 0;

	std::string_view name = this->name;

	if (ci_starts_with(name, query))
		return 400;
	// a word of the name starts with the query (words break on space,
	// '-', '_' and '.')
	for (size_t i = 1; i < name.size(); i++) {
		char prev = name[i - 1];
		if ((prev == ' ' || prev == '-' || prev == '_' || prev == '.')
				&& ci_starts_with(name.substr(i), query))
			return 300;
	}
	if (ci_contains(name, query))
		return 200;
	if (ci_contains(generic, query) || ci_contains(keywords, query))
		return 100;
	if (!exec.empty()) {
		size_t wl = exec.find(' ');
		std::string_view word = wl == std::string::npos
			? std::string_view(exec) : std::string_view(exec).substr(0, wl);
		size_t base = 0;
		for (size_t i = 0; i < word.size(); i++)
			if (word[i] == '/')
				base = i + 1;
		if (ci_contains(word.substr(base), query))
			return 50;
	}
	return -1;
}

std::optional<App> App::parse(std::string_view contents, std::string_view id,
	std::string_view lang) {
	if (size_t nul = contents.find('\0'); nul != std::string_view::npos)
		contents = contents.substr(0, nul); // a NUL ends the file
	const std::vector<std::string> lc = locale_variants(lang);

	Kvs kvs;
	bool in_group = false;
	size_t pos = 0;
	while (pos < contents.size()) {
		size_t eol = contents.find('\n', pos);
		std::string_view line = eol == std::string_view::npos
			? contents.substr(pos) : contents.substr(pos, eol - pos);
		pos = eol == std::string_view::npos ? contents.size() : eol + 1;
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1); // CRLF files

		std::string_view s = trim(line);
		if (s.empty() || s.front() == '#')
			continue;
		if (s.front() == '[') {
			if (!in_group && s == "[Desktop Entry]")
				in_group = true;
			else
				break; // the next group header ends the entry
		}
		if (!in_group)
			continue;
		size_t eq = s.find('=');
		if (eq == std::string_view::npos)
			continue;
		std::string_view key = trim(s.substr(0, eq));
		std::string_view val = trim(s.substr(eq + 1));
		if (!key.empty() && !val.empty())
			kvs.emplace_back(key, val);
	}

	const std::string *type = kv_get(kvs, "Type");
	const std::string *exec = kv_get(kvs, "Exec");
	const std::string *name = kv_localized(kvs, "Name", lc);
	if (!type || *type != "Application"
			|| bool_kv(kvs, "NoDisplay")
			|| bool_kv(kvs, "Hidden")
			|| !name || !exec)
		return std::nullopt;

	App app;
	app.id = id;
	app.name = unescape(*name);
	if (const std::string *generic = kv_localized(kvs, "GenericName", lc))
		app.generic = unescape(*generic);
	if (const std::string *keywords = kv_localized(kvs, "Keywords", lc))
		app.keywords = unescape(*keywords);
	app.exec = clean_exec(unescape(*exec));
	if (const std::string *icon = kv_get(kvs, "Icon"))
		app.icon = unescape(*icon);
	if (const std::string *try_exec = kv_get(kvs, "TryExec"))
		app.try_exec = unescape(*try_exec);
	app.terminal = bool_kv(kvs, "Terminal");
	return app;
}
