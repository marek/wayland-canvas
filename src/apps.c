#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "server.h"

#define APPS_MAX_FILE (256 * 1024)
#define APPS_MAX_DEPTH 4

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p)
		memcpy(p, s, n);
	return p;
}

static char *path_join(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	while (la > 0 && a[la - 1] == '/')
		la--;
	char *s = malloc(la + lb + 2);
	if (!s)
		return NULL;
	memcpy(s, a, la);
	s[la] = '/';
	memcpy(s + la + 1, b, lb + 1);
	return s;
}

/* ---- scanning state (apps_load only) ---- */

struct scan {
	struct cv_app *items;
	size_t count, cap;
	char **seen; /* desktop ids already handled, listed or not */
	size_t nseen, cseen;
	const char *locale;
};

static bool id_seen(struct scan *sc, const char *id)
{
	for (size_t i = 0; i < sc->nseen; i++)
		if (strcmp(sc->seen[i], id) == 0)
			return true;
	return false;
}

static void see_id(struct scan *sc, char *id)
{
	if (sc->nseen == sc->cseen) {
		size_t cap = sc->cseen ? sc->cseen * 2 : 64;
		char **seen = realloc(sc->seen, cap * sizeof(*seen));
		if (!seen) {
			free(id);
			return;
		}
		sc->seen = seen;
		sc->cseen = cap;
	}
	sc->seen[sc->nseen++] = id;
}

static void add_app(struct scan *sc, struct cv_app *app)
{
	if (sc->count == sc->cap) {
		size_t cap = sc->cap ? sc->cap * 2 : 64;
		struct cv_app *items = realloc(sc->items, cap * sizeof(*items));
		if (!items) {
			desktop_entry_free(app);
			return;
		}
		sc->items = items;
		sc->cap = cap;
	}
	sc->items[sc->count++] = *app;
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long sz = ftell(f);
	if (sz < 0 || sz > (long)APPS_MAX_FILE) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[sz] = '\0';
	fclose(f);
	return buf;
}

/* TryExec present? Absolute path: executable file. Else: on $PATH. */
static bool try_exec_ok(const char *te)
{
	if (!*te)
		return false;
	if (te[0] == '/')
		return access(te, X_OK) == 0;
	const char *path = getenv("PATH");
	if (!path || !*path)
		return false;
	const char *p = path;
	while (*p) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		if (len > 0) {
			char *dir = malloc(len + 1);
			if (dir) {
				memcpy(dir, p, len);
				dir[len] = '\0';
				char *cand = path_join(dir, te);
				free(dir);
				if (cand) {
					bool ok = access(cand, X_OK) == 0;
					free(cand);
					if (ok)
						return true;
				}
			}
		}
		if (!colon)
			break;
		p = colon + 1;
	}
	return false;
}

/* `base` is the applications root used for desktop-id computation; `depth`
 * 0 is the applications dir itself. */
static void scan_dir(struct scan *sc, const char *base, const char *dir, int depth)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		char *full = path_join(dir, ent->d_name);
		if (!full)
			continue;
		struct stat st;
		if (stat(full, &st) != 0) {
			free(full);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < APPS_MAX_DEPTH)
				scan_dir(sc, base, full, depth + 1);
			free(full);
			continue;
		}
		size_t namelen = strlen(ent->d_name);
		if (!S_ISREG(st.st_mode) || namelen < 8
				|| strcmp(ent->d_name + namelen - 8, ".desktop") != 0) {
			free(full);
			continue;
		}

		/* desktop id: path below the applications dir, '/' -> '-' */
		const char *rel = full + strlen(base);
		while (*rel == '/')
			rel++;
		char *id = xstrdup(rel);
		if (!id) {
			free(full);
			continue;
		}
		for (char *c = id; *c; c++)
			if (*c == '/')
				*c = '-';
		/* the first occurrence of an id wins, even when it is hidden */
		if (id_seen(sc, id)) {
			free(id);
			free(full);
			continue;
		}

		struct cv_app app;
		char *contents = read_file(full);
		if (contents && desktop_entry_parse(contents, id, sc->locale, &app)) {
			if (app.try_exec && !try_exec_ok(app.try_exec))
				desktop_entry_free(&app);
			else
				add_app(sc, &app);
		}
		free(contents);
		see_id(sc, id);
		free(full);
	}
	closedir(d);
}

static int app_cmp(const void *a, const void *b)
{
	const struct cv_app *x = a, *y = b;
	int c = strcasecmp(x->name ? x->name : "", y->name ? y->name : "");
	if (c)
		return c;
	return strcasecmp(x->id ? x->id : "", y->id ? y->id : "");
}

void apps_load(struct cv_apps *apps)
{
	apps->items = NULL;
	apps->count = 0;

	const char *locale = getenv("LC_ALL");
	if (!locale || !*locale)
		locale = getenv("LC_MESSAGES");
	if (!locale || !*locale)
		locale = getenv("LANG");

	struct scan sc = {0};
	sc.locale = locale;

	/* $XDG_DATA_HOME/applications, then each $XDG_DATA_DIRS/applications */
	size_t ndirs = 0;
	char **dirs = NULL;
	const char *x = getenv("XDG_DATA_HOME");
	char *data_home = NULL;
	if (x && *x) {
		data_home = xstrdup(x);
	} else {
		const char *home = getenv("HOME");
		if (home)
			data_home = path_join(home, ".local/share");
	}
	if (data_home) {
		char *dir = path_join(data_home, "applications");
		if (dir) {
			char **grown = realloc(dirs, (ndirs + 1) * sizeof(*dirs));
			if (grown) {
				dirs = grown;
				dirs[ndirs++] = dir;
			} else {
				free(dir);
			}
		}
	}
	const char *dd = getenv("XDG_DATA_DIRS");
	if (!dd || !*dd)
		dd = "/usr/local/share:/usr/share";
	const char *p = dd;
	while (*p) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		if (len > 0) {
			char *base = malloc(len + 1);
			if (base) {
				memcpy(base, p, len);
				base[len] = '\0';
				char *dir = path_join(base, "applications");
				free(base);
				if (dir) {
					char **grown = realloc(dirs, (ndirs + 1) * sizeof(*dirs));
					if (grown) {
						dirs = grown;
						dirs[ndirs++] = dir;
					} else {
						free(dir);
					}
				}
			}
		}
		if (!colon)
			break;
		p = colon + 1;
	}

	for (size_t i = 0; i < ndirs; i++)
		scan_dir(&sc, dirs[i], dirs[i], 0);

	for (size_t i = 0; i < ndirs; i++)
		free(dirs[i]);
	free(dirs);
	free(data_home);

	if (sc.count > 1)
		qsort(sc.items, sc.count, sizeof(*sc.items), app_cmp);
	apps->items = sc.items;
	apps->count = sc.count;

	for (size_t i = 0; i < sc.nseen; i++)
		free(sc.seen[i]);
	free(sc.seen);
}

void apps_free(struct cv_apps *apps)
{
	if (!apps)
		return;
	for (size_t i = 0; i < apps->count; i++)
		desktop_entry_free(&apps->items[i]);
	free(apps->items);
	apps->items = NULL;
	apps->count = 0;
}

/* ---- search ---- */

struct hit {
	size_t idx;
	int score;
};

static int hit_cmp(const void *a, const void *b, const struct cv_app *items)
{
	const struct hit *x = a, *y = b;
	if (x->score != y->score)
		return y->score - x->score; /* better match first */
	int c = strcasecmp(items[x->idx].name ? items[x->idx].name : "",
		items[y->idx].name ? items[y->idx].name : "");
	if (c)
		return c;
	return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

size_t apps_search(const struct cv_apps *apps, const char *query, size_t *out, size_t max)
{
	if (!apps || !out || max == 0)
		return 0;

	if (!query || !*query) {
		size_t n = apps->count < max ? apps->count : max;
		for (size_t i = 0; i < n; i++)
			out[i] = i;
		return n;
	}

	struct hit *hits = malloc(apps->count * sizeof(*hits));
	if (!hits)
		return 0;
	size_t n = 0;
	for (size_t i = 0; i < apps->count; i++) {
		int score = desktop_entry_match(&apps->items[i], query);
		if (score >= 0) {
			hits[n].idx = i;
			hits[n].score = score;
			n++;
		}
	}

	/* insertion sort by hit_cmp: n is small and the comparator needs `items` */
	for (size_t i = 1; i < n; i++) {
		struct hit h = hits[i];
		size_t j = i;
		while (j > 0 && hit_cmp(&h, &hits[j - 1], apps->items) < 0) {
			hits[j] = hits[j - 1];
			j--;
		}
		hits[j] = h;
	}

	if (n > max)
		n = max;
	for (size_t i = 0; i < n; i++)
		out[i] = hits[i].idx;
	free(hits);
	return n;
}

/* ---- launching ---- */

void app_launch(const struct cv_app *app)
{
	if (!app || !app->exec || !*app->exec)
		return;
	char *cmd;
	if (app->terminal) {
		size_t n = strlen(TERMINAL_CMD) + strlen(app->exec) + 8;
		cmd = malloc(n);
		if (!cmd)
			return;
		snprintf(cmd, n, "%s -e %s", TERMINAL_CMD, app->exec);
	} else {
		size_t n = strlen(app->exec) + 8;
		cmd = malloc(n);
		if (!cmd)
			return;
		snprintf(cmd, n, "exec %s", app->exec);
	}
	server_spawn(cmd);
	free(cmd);
}
