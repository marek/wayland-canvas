#include <string>
#include <string_view>
#include <vector>
#include "apps/desktop_entry.h"
#include "core/text.h"

namespace {

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

bool bool_key(const text::IniGroup &entry, std::string_view key) {
	const std::string *v = entry.get(key);
	return v && text::ci_equal(*v, "true");
}

// Locale keys for `lang` in spec order (de_DE@euro, de_DE, de@euro, de); none for "", "C" and "POSIX".
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
const std::string *kv_localized(const text::IniGroup &entry, std::string_view key,
	const std::vector<std::string> &lc) {
	for (const std::string &v : lc) {
		std::string localized = std::string(key) + '[' + v + ']';
		if (const std::string *val = entry.get(localized))
			return val;
	}
	return entry.get(key);
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

	if (text::ci_starts_with(name, query))
		return 400;
	// a word of the name starts with the query (words break on space,
	// '-', '_' and '.')
	for (size_t i = 1; i < name.size(); i++) {
		char prev = name[i - 1];
		if ((prev == ' ' || prev == '-' || prev == '_' || prev == '.')
				&& text::ci_starts_with(name.substr(i), query))
			return 300;
	}
	if (text::ci_contains(name, query))
		return 200;
	if (text::ci_contains(generic, query) || text::ci_contains(keywords, query))
		return 100;
	if (!exec.empty()) {
		size_t wl = exec.find(' ');
		std::string_view word = wl == std::string::npos
			? std::string_view(exec) : std::string_view(exec).substr(0, wl);
		size_t base = 0;
		for (size_t i = 0; i < word.size(); i++)
			if (word[i] == '/')
				base = i + 1;
		if (text::ci_contains(word.substr(base), query))
			return 50;
	}
	return -1;
}

std::optional<App> App::parse(std::string_view contents, std::string_view id,
	std::string_view lang) {
	const text::Ini ini = text::parse_ini(contents);
	// the spec makes [Desktop Entry] the file's first group; the groups after it
	// ([Desktop Action ...]) describe actions, not the entry
	if (ini.groups.empty() || ini.groups.front().name != "Desktop Entry")
		return std::nullopt;
	const text::IniGroup &entry = ini.groups.front();
	const std::vector<std::string> lc = locale_variants(lang);

	const std::string *type = entry.get("Type");
	const std::string *exec = entry.get("Exec");
	const std::string *name = kv_localized(entry, "Name", lc);
	if (!type || *type != "Application"
			|| bool_key(entry, "NoDisplay")
			|| bool_key(entry, "Hidden")
			|| !name || !exec)
		return std::nullopt;

	App app;
	app.id = id;
	app.name = unescape(*name);
	if (const std::string *generic = kv_localized(entry, "GenericName", lc))
		app.generic = unescape(*generic);
	if (const std::string *keywords = kv_localized(entry, "Keywords", lc))
		app.keywords = unescape(*keywords);
	app.exec = clean_exec(unescape(*exec));
	if (const std::string *icon = entry.get("Icon"))
		app.icon = unescape(*icon);
	if (const std::string *try_exec = entry.get("TryExec"))
		app.try_exec = unescape(*try_exec);
	app.terminal = bool_key(entry, "Terminal");
	return app;
}
