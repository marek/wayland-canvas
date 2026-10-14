#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "desktop_entry.h"

/* ---- small helpers ---- */

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p)
		memcpy(p, s, n);
	return p;
}

/* Trims leading/trailing spaces and tabs in place; returns the start. */
static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
	return s;
}

static bool bool_val(const char *v)
{
	return v && strcasecmp(v, "true") == 0;
}

static char ascii_lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
}

static bool ci_starts_with(const char *s, const char *q, size_t qlen)
{
	for (size_t i = 0; i < qlen; i++)
		if (ascii_lower(s[i]) != ascii_lower(q[i]))
			return false; /* s[i] == '\0' mismatches: q has no NUL */
	return true;
}

static bool ci_contains(const char *s, size_t n, const char *q, size_t qlen)
{
	if (!s || qlen == 0 || n < qlen)
		return false;
	for (size_t i = 0; i + qlen <= n; i++)
		if (ci_starts_with(s + i, q, qlen))
			return true;
	return false;
}

static char *unescape(const char *v)
{
	char *out = malloc(strlen(v) + 1);
	if (!out)
		return NULL;
	size_t o = 0;
	for (size_t i = 0; v[i]; i++) {
		if (v[i] != '\\') {
			out[o++] = v[i];
			continue;
		}
		switch (v[i + 1]) {
		case 's': out[o++] = ' ';  i++; break;
		case 'n': out[o++] = '\n'; i++; break;
		case 't': out[o++] = '\t'; i++; break;
		case 'r': out[o++] = '\r'; i++; break;
		case '\\': out[o++] = '\\'; i++; break;
		default: out[o++] = v[i]; break; /* unknown escape: keep backslash */
		}
	}
	out[o] = '\0';
	return out;
}

/* ---- key/value storage for one file ---- */

struct kv {
	char *key; /* "Name" or "Name[de_DE]" */
	char *val; /* raw value, already trimmed, never empty */
};

struct kvs {
	struct kv *items;
	size_t count, cap;
};

static void kv_add(struct kvs *k, char *key, char *val)
{
	if (k->count == k->cap) {
		size_t cap = k->cap ? k->cap * 2 : 32;
		struct kv *items = realloc(k->items, cap * sizeof(*items));
		if (!items)
			return;
		k->items = items;
		k->cap = cap;
	}
	char *kcopy = xstrdup(key);
	char *vcopy = xstrdup(val);
	if (!kcopy || !vcopy) {
		free(kcopy);
		free(vcopy);
		return;
	}
	k->items[k->count].key = kcopy;
	k->items[k->count].val = vcopy;
	k->count++;
}

static void kv_free(struct kvs *k)
{
	for (size_t i = 0; i < k->count; i++) {
		free(k->items[i].key);
		free(k->items[i].val);
	}
	free(k->items);
}

static const char *kv_get(const struct kvs *k, const char *key)
{
	for (size_t i = 0; i < k->count; i++)
		if (strcmp(k->items[i].key, key) == 0)
			return k->items[i].val;
	return NULL;
}

/*
 * Locale keys in spec order for `lang` (e.g. "de_DE.UTF-8@euro"):
 * de_DE@euro, de_DE, de@euro, de. NULL/"C"/"POSIX" localize nothing.
 * Returns 0..4, `out[i]` filled.
 */
/* Room for lang_COUNTRY@MODIFIER built from a lang string truncated to 63 bytes. */
#define LOCALE_MAX 192

static int locale_variants(const char *lang, char out[4][LOCALE_MAX])
{
	if (!lang || !*lang || strcmp(lang, "C") == 0 || strcmp(lang, "POSIX") == 0)
		return 0;

	char buf[64];
	snprintf(buf, sizeof(buf), "%s", lang);

	char *dot = strchr(buf, '.');
	if (dot)
		*dot = '\0'; /* drop .encoding */
	char *at = strchr(buf, '@');
	char *mod = at ? at + 1 : NULL;
	if (at)
		*at = '\0';
	char *us = strchr(buf, '_');
	char *country = us ? us + 1 : NULL;
	if (us)
		*us = '\0';
	if (!*buf)
		return 0;

	int n = 0;
	if (country && mod) {
		snprintf(out[n++], LOCALE_MAX, "%s_%s@%s", buf, country, mod);
	}
	if (country) {
		snprintf(out[n++], LOCALE_MAX, "%s_%s", buf, country);
	}
	if (mod) {
		snprintf(out[n++], LOCALE_MAX, "%s@%s", buf, mod);
	}
	snprintf(out[n++], LOCALE_MAX, "%s", buf);
	return n;
}

/* Best value for `key`: Name[locale] variants in order, then plain Name. */
static const char *kv_localized(const struct kvs *k, const char *key,
	const char lc[4][LOCALE_MAX], int nlc)
{
	char buf[LOCALE_MAX + 64];
	for (int i = 0; i < nlc; i++) {
		snprintf(buf, sizeof(buf), "%s[%s]", key, lc[i]);
		const char *v = kv_get(k, buf);
		if (v)
			return v;
	}
	return kv_get(k, key);
}

/* ---- Exec cleaning ---- */

static bool is_field_code(char c)
{
	return strchr("fFuUdDnNiIckvm", c) != NULL;
}

char *desktop_exec_clean(const char *exec)
{
	if (!exec)
		return NULL;

	size_t n = strlen(exec);
	char *out = malloc(n + 1);
	if (!out)
		return NULL;
	size_t o = 0;
	for (size_t i = 0; i < n;) {
		char c = exec[i];
		/* a quoted argument that is only a field code: drop the quotes too */
		if (c == '"' && i + 3 < n && exec[i + 1] == '%'
				&& is_field_code(exec[i + 2]) && exec[i + 3] == '"') {
			i += 4;
			continue;
		}
		if (c == '%' && i + 1 < n) {
			if (exec[i + 1] == '%') {
				out[o++] = '%';
				i += 2;
				continue;
			}
			if (is_field_code(exec[i + 1])) {
				i += 2;
				continue;
			}
		}
		out[o++] = c;
		i++;
	}

	/* collapse runs of spaces to one, trim the ends */
	size_t w = 0;
	bool last_space = true;
	for (size_t i = 0; i < o; i++) {
		if (out[i] == ' ') {
			if (!last_space) {
				out[w++] = ' ';
				last_space = true;
			}
		} else {
			out[w++] = out[i];
			last_space = false;
		}
	}
	if (w > 0 && out[w - 1] == ' ')
		w--;
	out[w] = '\0';
	return out;
}

/* ---- matching ---- */

int desktop_entry_match(const struct cv_app *app, const char *query)
{
	if (!app)
		return query && *query ? -1 : 0;
	if (!query || !*query)
		return 0;

	size_t qlen = strlen(query);
	const char *name = app->name ? app->name : "";

	if (ci_starts_with(name, query, qlen))
		return 400;
	/* a word of the name starts with the query (words break on space,
	 * '-', '_' and '.') */
	for (size_t i = 1; name[i]; i++) {
		char prev = name[i - 1];
		if ((prev == ' ' || prev == '-' || prev == '_' || prev == '.')
				&& ci_starts_with(name + i, query, qlen))
			return 300;
	}
	if (ci_contains(name, strlen(name), query, qlen))
		return 200;
	if (ci_contains(app->generic, app->generic ? strlen(app->generic) : 0, query, qlen)
			|| ci_contains(app->keywords, app->keywords ? strlen(app->keywords) : 0, query, qlen))
		return 100;
	if (app->exec) {
		size_t wl = strcspn(app->exec, " ");
		const char *base = app->exec;
		for (size_t i = 0; i < wl; i++)
			if (app->exec[i] == '/')
				base = app->exec + i + 1;
		if (ci_contains(base, wl - (size_t)(base - app->exec), query, qlen))
			return 50;
	}
	return -1;
}

/* ---- parsing ---- */

bool desktop_entry_parse(const char *contents, const char *id, const char *lang,
	struct cv_app *app)
{
	memset(app, 0, sizeof(*app));
	if (!contents)
		return false;

	char lc[4][LOCALE_MAX];
	int nlc = locale_variants(lang, lc);

	struct kvs k = {0};
	bool in_group = false;
	const char *p = contents;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char *line = malloc(len + 1);
		if (!line)
			break;
		memcpy(line, p, len);
		if (len > 0 && line[len - 1] == '\r')
			len--; /* CRLF files */
		line[len] = '\0';
		p = eol ? eol + 1 : p + len;

		char *s = trim(line);
		if (*s == '\0' || *s == '#') {
			free(line);
			continue;
		}
		if (*s == '[') {
			if (!in_group && strcmp(s, "[Desktop Entry]") == 0) {
				in_group = true;
			} else {
				free(line);
				break; /* the next group header ends the entry */
			}
		}
		if (!in_group) {
			free(line);
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) {
			free(line);
			continue;
		}
		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);
		if (*key && *val)
			kv_add(&k, key, val);
		free(line);
	}

	const char *type = kv_get(&k, "Type");
	const char *exec = kv_get(&k, "Exec");
	const char *name = kv_localized(&k, "Name", lc, nlc);
	if (!type || strcmp(type, "Application") != 0
			|| bool_val(kv_get(&k, "NoDisplay"))
			|| bool_val(kv_get(&k, "Hidden"))
			|| !name || !exec) {
		kv_free(&k);
		memset(app, 0, sizeof(*app));
		return false;
	}

	app->id = id ? xstrdup(id) : NULL;
	app->name = unescape(name);
	const char *generic = kv_localized(&k, "GenericName", lc, nlc);
	if (generic)
		app->generic = unescape(generic);
	const char *keywords = kv_localized(&k, "Keywords", lc, nlc);
	if (keywords)
		app->keywords = unescape(keywords);
	char *raw_exec = unescape(exec);
	app->exec = raw_exec ? desktop_exec_clean(raw_exec) : NULL;
	free(raw_exec);
	const char *icon = kv_get(&k, "Icon");
	if (icon)
		app->icon = unescape(icon);
	const char *try_exec = kv_get(&k, "TryExec");
	if (try_exec)
		app->try_exec = unescape(try_exec);
	app->terminal = bool_val(kv_get(&k, "Terminal"));

	kv_free(&k);
	if (!app->name || !app->exec) {
		desktop_entry_free(app);
		return false;
	}
	return true;
}

void desktop_entry_free(struct cv_app *app)
{
	if (!app)
		return;
	free(app->id);
	free(app->name);
	free(app->generic);
	free(app->keywords);
	free(app->exec);
	free(app->icon);
	free(app->try_exec);
	memset(app, 0, sizeof(*app));
}
