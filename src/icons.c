#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <glib.h>
#include <librsvg/rsvg.h>
#include "server.h"

/* Only these directory kinds matter here; per-theme data and the loaded
 * surfaces are cached for the process lifetime and freed by icons_finish. */

enum icon_dir_type {
	ICON_DIR_FIXED,
	ICON_DIR_SCALABLE,
	ICON_DIR_THRESHOLD,
};

struct icon_dir {
	char *name; /* path inside the theme, e.g. "48x48/apps" */
	int size;
	int scale;
	int min_size, max_size;
	int threshold;
	enum icon_dir_type type;
	bool apps; /* Context=Applications (or "apps" in the path): searched first */
};

struct icon_theme {
	char *name;
	struct icon_dir *dirs;
	size_t ndirs, dcap;
	char **inherits;
	size_t ninherits, icap;
};

struct icon_cache {
	struct icon_cache *next;
	char *icon; /* key: the original name or path */
	int size;
	cairo_surface_t *surface; /* NULL = looked up and not found */
};

static struct icon_theme *themes;
static size_t nthemes, tcap;

static char **chain; /* theme names: user theme, its Inherits..., hicolor */
static size_t nchain;
static bool chain_built;

static char **base_dirs;
static size_t nbase_dirs, bcap;

static struct icon_cache *cache;

#define ICONS_MAX_CHAIN 8

/* ---- small helpers ---- */

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
	if (lb > 0 && b[0] == '/')
		b++, lb--;
	char *s = malloc(la + lb + 2);
	if (!s)
		return NULL;
	memcpy(s, a, la);
	s[la] = '/';
	memcpy(s + la + 1, b, lb + 1);
	return s;
}

static char ascii_lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
}

static bool ci_contains(const char *s, const char *needle)
{
	size_t nl = strlen(needle);
	if (!s || nl == 0)
		return false;
	for (const char *p = s; *p; p++) {
		size_t i = 0;
		while (i < nl && p[i] && ascii_lower(p[i]) == ascii_lower(needle[i]))
			i++;
		if (i == nl)
			return true;
	}
	return false;
}

/* Strips surrounding whitespace and quotes in place (gsettings output,
 * settings.ini values). */
static void strip_quotes(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '
			|| s[n - 1] == '\t' || s[n - 1] == '\'' || s[n - 1] == '"'))
		s[--n] = '\0';
	size_t off = 0;
	while (s[off] == ' ' || s[off] == '\t' || s[off] == '\'' || s[off] == '"')
		off++;
	if (off > 0)
		memmove(s, s + off, strlen(s + off) + 1);
}

/* ---- base dirs ---- */

static void add_base_dir(const char *path)
{
	if (!path || !*path)
		return;
	char *dir = path_join(path, "icons");
	if (!dir)
		return;
	for (size_t i = 0; i < nbase_dirs; i++)
		if (strcmp(base_dirs[i], dir) == 0) {
			free(dir);
			return;
		}
	if (nbase_dirs == bcap) {
		size_t cap = bcap ? bcap * 2 : 8;
		char **grown = realloc(base_dirs, cap * sizeof(*grown));
		if (!grown) {
			free(dir);
			return;
		}
		base_dirs = grown;
		bcap = cap;
	}
	base_dirs[nbase_dirs++] = dir;
}

static void ensure_base_dirs(void)
{
	if (base_dirs)
		return;
	const char *home = getenv("HOME");

	const char *x = getenv("XDG_DATA_HOME");
	if (x && *x) {
		add_base_dir(x);
	} else if (home) {
		char *dh = path_join(home, ".local/share");
		if (dh) {
			add_base_dir(dh);
			free(dh);
		}
	}
	if (home) {
		char *icons = path_join(home, ".icons");
		if (icons) {
			add_base_dir(icons);
			free(icons);
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
				add_base_dir(base);
				free(base);
			}
		}
		if (!colon)
			break;
		p = colon + 1;
	}
}

/* ---- theme detection (gsettings, then GTK settings.ini) ---- */

static char *theme_from_ini(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	static const char key[] = "gtk-icon-theme-name";
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, key, sizeof(key) - 1) != 0)
			continue;
		s += sizeof(key) - 1;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s != '=')
			continue;
		s++;
		while (*s == ' ' || *s == '\t')
			s++;
		strip_quotes(s);
		if (*s) {
			fclose(f);
			return xstrdup(s);
		}
	}
	fclose(f);
	return NULL;
}

static char *detect_theme(void)
{
	FILE *f = popen("gsettings get org.gnome.desktop.interface icon-theme", "r");
	if (f) {
		char buf[256] = "";
		bool got = fgets(buf, sizeof(buf), f) != NULL;
		int status = pclose(f);
		if (got && status == 0) {
			strip_quotes(buf);
			if (*buf)
				return xstrdup(buf);
		}
	}

	char *base = NULL;
	const char *x = getenv("XDG_CONFIG_HOME");
	if (x && *x) {
		base = xstrdup(x);
	} else {
		const char *home = getenv("HOME");
		if (home)
			base = path_join(home, ".config");
	}
	char *theme = NULL;
	if (base) {
		static const char *vers[2] = {"gtk-4.0", "gtk-3.0"};
		for (size_t i = 0; i < 2 && !theme; i++) {
			char *cfg_dir = path_join(base, vers[i]);
			if (!cfg_dir)
				continue;
			char *ini = path_join(cfg_dir, "settings.ini");
			free(cfg_dir);
			if (!ini)
				continue;
			theme = theme_from_ini(ini);
			free(ini);
		}
		free(base);
	}
	return theme;
}

/* ---- index.theme parsing ---- */

static int kf_int(GKeyFile *kf, const char *group, const char *key, int def)
{
	GError *err = NULL;
	gint v = g_key_file_get_integer(kf, group, key, &err);
	if (err) {
		g_error_free(err);
		return def;
	}
	return (int)v;
}

/* Application directories are searched first; the rest only when an app's
 * icon lives elsewhere (e.g. Avahi's "network-wired" under devices). */
static void add_icon_dir(struct icon_theme *th, GKeyFile *kf, const char *dir)
{
	GError *err = NULL;
	gint size = g_key_file_get_integer(kf, dir, "Size", &err);
	if (err) {
		g_error_free(err);
		return; /* Size is required by the spec */
	}

	struct icon_dir d;
	memset(&d, 0, sizeof(d));
	d.size = (int)size;
	d.scale = kf_int(kf, dir, "Scale", 1);
	if (d.scale < 1)
		d.scale = 1;
	d.min_size = kf_int(kf, dir, "MinSize", d.size);
	d.max_size = kf_int(kf, dir, "MaxSize", d.size);
	d.threshold = kf_int(kf, dir, "Threshold", 2);
	if (d.threshold < 0)
		d.threshold = 0;

	gchar *type = g_key_file_get_string(kf, dir, "Type", NULL);
	if (type && strcasecmp(type, "Fixed") == 0)
		d.type = ICON_DIR_FIXED;
	else if (type && strcasecmp(type, "Scalable") == 0)
		d.type = ICON_DIR_SCALABLE;
	else
		d.type = ICON_DIR_THRESHOLD;
	if (type)
		g_free(type);

	gchar *ctx = g_key_file_get_string(kf, dir, "Context", NULL);
	d.apps = (ctx && strcasecmp(ctx, "Applications") == 0)
		|| ci_contains(dir, "apps");
	if (ctx)
		g_free(ctx);

	d.name = xstrdup(dir);
	if (!d.name)
		return;
	if (th->ndirs == th->dcap) {
		size_t cap = th->dcap ? th->dcap * 2 : 16;
		struct icon_dir *grown = realloc(th->dirs, cap * sizeof(*grown));
		if (!grown) {
			free(d.name);
			return;
		}
		th->dirs = grown;
		th->dcap = cap;
	}
	th->dirs[th->ndirs++] = d;
}

static void add_inherit(struct icon_theme *th, const char *name)
{
	if (!*name)
		return;
	if (th->ninherits == th->icap) {
		size_t cap = th->icap ? th->icap * 2 : 4;
		char **grown = realloc(th->inherits, cap * sizeof(*grown));
		if (!grown)
			return;
		th->inherits = grown;
		th->icap = cap;
	}
	char *copy = xstrdup(name);
	if (copy)
		th->inherits[th->ninherits++] = copy;
}

static void load_theme_index(struct icon_theme *th)
{
	ensure_base_dirs();
	for (size_t b = 0; b < nbase_dirs; b++) {
		char *theme_dir = path_join(base_dirs[b], th->name);
		if (!theme_dir)
			continue;
		char *idx = path_join(theme_dir, "index.theme");
		free(theme_dir);
		if (!idx)
			continue;
		GKeyFile *kf = g_key_file_new();
		// index.theme lists (Inherits, Directories) are comma-separated.
		g_key_file_set_list_separator(kf, ',');
		GError *err = NULL;
		if (g_key_file_load_from_file(kf, idx, G_KEY_FILE_NONE, &err)) {
			gchar **inh = g_key_file_get_string_list(kf, "Icon Theme",
				"Inherits", NULL, NULL);
			if (inh) {
				for (gchar **i = inh; *i; i++)
					add_inherit(th, *i);
				g_strfreev(inh);
			}
			gchar **dirs = g_key_file_get_string_list(kf, "Icon Theme",
				"Directories", NULL, NULL);
			if (dirs) {
				for (gchar **i = dirs; *i; i++)
					add_icon_dir(th, kf, *i);
				g_strfreev(dirs);
			}
			gchar **sdirs = g_key_file_get_string_list(kf, "Icon Theme",
				"ScaledDirectories", NULL, NULL);
			if (sdirs) {
				for (gchar **i = sdirs; *i; i++)
					add_icon_dir(th, kf, *i);
				g_strfreev(sdirs);
			}
			g_key_file_free(kf);
			free(idx);
			return; /* first index.theme found wins */
		}
		g_error_free(err);
		g_key_file_free(kf);
		free(idx);
	}
}

static struct icon_theme *theme_get(const char *name)
{
	for (size_t i = 0; i < nthemes; i++)
		if (themes[i].name && strcmp(themes[i].name, name) == 0)
			return &themes[i];
	if (nthemes == tcap) {
		size_t cap = tcap ? tcap * 2 : 8;
		struct icon_theme *grown = realloc(themes, cap * sizeof(*grown));
		if (!grown)
			return NULL;
		themes = grown;
		tcap = cap;
	}
	struct icon_theme *th = &themes[nthemes];
	memset(th, 0, sizeof(*th));
	th->name = xstrdup(name);
	nthemes++;
	if (th->name)
		load_theme_index(th);
	return th;
}

/* ---- theme chain ---- */

static void chain_push(const char *name)
{
	for (size_t i = 0; i < nchain; i++)
		if (strcmp(chain[i], name) == 0)
			return;
	if (nchain >= ICONS_MAX_CHAIN)
		return;
	char **grown = realloc(chain, (nchain + 1) * sizeof(*grown));
	if (!grown)
		return;
	chain = grown;
	char *copy = xstrdup(name);
	if (copy)
		chain[nchain++] = copy;
}

static void build_chain(void)
{
	if (chain_built)
		return;
	chain_built = true;
	char *t = detect_theme();
	if (t) {
		chain_push(t);
		free(t);
	}
	for (size_t i = 0; i < nchain && nchain < ICONS_MAX_CHAIN; i++) {
		struct icon_theme *th = theme_get(chain[i]);
		if (!th)
			continue;
		for (size_t j = 0; j < th->ninherits && nchain < ICONS_MAX_CHAIN; j++)
			chain_push(th->inherits[j]);
	}
	chain_push("hicolor");
}

/* ---- lookup ---- */

/* 0 when the directory covers `want` (exactly, Scalable range or within the
 * threshold); otherwise the distance to the nearest edge. */
static int dir_distance(const struct icon_dir *d, int want)
{
	int base = d->size * d->scale;
	int lo = base, hi = base;
	if (d->type == ICON_DIR_SCALABLE) {
		lo = d->min_size * d->scale;
		hi = d->max_size * d->scale;
	} else if (d->type == ICON_DIR_THRESHOLD) {
		lo = base - d->threshold * d->scale;
		hi = base + d->threshold * d->scale;
	}
	if (want >= lo && want <= hi)
		return 0;
	return want < lo ? lo - want : want - hi;
}

/* First existing <base>/<theme>/<dir>/<name><ext> across the base dirs. */
static char *find_in_dir(const char *theme, const struct icon_dir *dir,
	const char *fname)
{
	for (int e = 0; e < 2; e++) {
		const char *ext = e == 0 ? ".png" : ".svg";
		char *full = malloc(strlen(fname) + strlen(ext) + 1);
		if (!full)
			continue;
		sprintf(full, "%s%s", fname, ext);
		for (size_t b = 0; b < nbase_dirs; b++) {
			char *theme_dir = path_join(base_dirs[b], theme);
			if (!theme_dir)
				continue;
			char *dir_path = path_join(theme_dir, dir->name);
			free(theme_dir);
			if (!dir_path)
				continue;
			char *cand = path_join(dir_path, full);
			free(dir_path);
			if (!cand)
				continue;
			if (access(cand, R_OK) == 0) {
				free(full);
				return cand;
			}
			free(cand);
		}
		free(full);
	}
	return NULL;
}

static cairo_surface_t *load_file_icon(const char *path, int size);

static cairo_surface_t *find_theme_icon(const char *name, int size)
{
	build_chain();
	for (int pass = 0; pass < 2; pass++) {
		bool apps_only = pass == 0;
		for (size_t t = 0; t < nchain; t++) {
			struct icon_theme *th = theme_get(chain[t]);
			if (!th)
				continue;
			char *best = NULL;
			int best_dist = INT_MAX;
			for (size_t d = 0; d < th->ndirs; d++) {
				if (th->dirs[d].apps != apps_only)
					continue;
				int dist = dir_distance(&th->dirs[d], size);
				if (dist > best_dist)
					continue;
				char *cand = find_in_dir(chain[t], &th->dirs[d], name);
				if (!cand)
					continue;
				if (dist < best_dist) {
					free(best);
					best = cand;
					best_dist = dist;
				} else {
					free(cand);
				}
			}
			if (best) {
				cairo_surface_t *s = load_file_icon(best, size);
				free(best);
				return s; /* first theme with any hit wins */
			}
		}
	}
	return NULL;
}

/* ---- loading ---- */

/* The image scaled to fit `size` x `size` (aspect kept, centred). */
static cairo_surface_t *scale_to_fit(cairo_surface_t *img, int size)
{
	int w = cairo_image_surface_get_width(img);
	int h = cairo_image_surface_get_height(img);
	if (w <= 0 || h <= 0)
		return NULL;
	double s = (double)size / (double)(w > h ? w : h);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		size, size);
	if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(out);
		return NULL;
	}
	cairo_t *cr = cairo_create(out);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, img, (size / s - w) / 2.0, (size / s - h) / 2.0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return out;
}

static cairo_surface_t *load_png(const char *path, int size)
{
	cairo_surface_t *img = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(img);
		return NULL;
	}
	cairo_surface_t *out = scale_to_fit(img, size);
	cairo_surface_destroy(img);
	return out;
}

static cairo_surface_t *load_svg(const char *path, int size)
{
	GError *err = NULL;
	RsvgHandle *handle = rsvg_handle_new_from_file(path, &err);
	if (!handle) {
		if (err)
			g_error_free(err);
		return NULL;
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		size, size);
	if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(out);
		g_object_unref(handle);
		return NULL;
	}
	cairo_t *cr = cairo_create(out);
	RsvgRectangle viewport = {0, 0, size, size};
	bool ok = rsvg_handle_render_document(handle, cr, &viewport, &err);
	cairo_destroy(cr);
	g_object_unref(handle);
	if (!ok) {
		if (err)
			g_error_free(err);
		cairo_surface_destroy(out);
		return NULL;
	}
	return out;
}

static cairo_surface_t *load_file_icon(const char *path, int size)
{
	const char *dot = strrchr(path, '.');
	if (dot && strcasecmp(dot, ".svg") == 0)
		return load_svg(path, size);
	if (dot && strcasecmp(dot, ".png") == 0)
		return load_png(path, size);
	cairo_surface_t *s = load_png(path, size);
	return s ? s : load_svg(path, size);
}

static cairo_surface_t *load_icon(const char *icon, int size)
{
	if (icon[0] == '/')
		return load_file_icon(icon, size);

	/* theme icon: drop a known extension from the name */
	const char *dot = strrchr(icon, '.');
	char namebuf[512];
	const char *name = icon;
	if (dot && (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".svg") == 0
			|| strcasecmp(dot, ".xpm") == 0)) {
		size_t n = (size_t)(dot - icon);
		if (n >= sizeof(namebuf))
			n = sizeof(namebuf) - 1;
		memcpy(namebuf, icon, n);
		namebuf[n] = '\0';
		name = namebuf;
		if (!*name)
			return NULL;
	}

	cairo_surface_t *s = find_theme_icon(name, size);
	if (s)
		return s;

	/* last resort: /usr/share/pixmaps */
	for (int e = 0; e < 2; e++) {
		const char *ext = e == 0 ? ".png" : ".svg";
		char *fname = malloc(strlen(name) + strlen(ext) + 1);
		if (!fname)
			continue;
		sprintf(fname, "%s%s", name, ext);
		char *path = path_join("/usr/share/pixmaps", fname);
		free(fname);
		if (!path)
			continue;
		if (access(path, R_OK) == 0)
			s = load_file_icon(path, size);
		free(path);
		if (s)
			return s;
	}
	return NULL;
}

cairo_surface_t *icon_lookup(const char *icon, int size)
{
	if (!icon || !*icon || size <= 0)
		return NULL;
	for (struct icon_cache *c = cache; c; c = c->next)
		if (c->size == size && strcmp(c->icon, icon) == 0)
			return c->surface;

	cairo_surface_t *surface = load_icon(icon, size);
	struct icon_cache *c = malloc(sizeof(*c));
	if (c) {
		c->icon = xstrdup(icon);
		c->size = size;
		c->surface = surface;
		if (c->icon) {
			c->next = cache;
			cache = c;
			return surface;
		}
		free(c);
	}
	return surface;
}

void icons_finish(void)
{
	while (cache) {
		struct icon_cache *c = cache;
		cache = c->next;
		if (c->surface)
			cairo_surface_destroy(c->surface);
		free(c->icon);
		free(c);
	}

	for (size_t i = 0; i < nthemes; i++) {
		for (size_t d = 0; d < themes[i].ndirs; d++)
			free(themes[i].dirs[d].name);
		free(themes[i].dirs);
		for (size_t j = 0; j < themes[i].ninherits; j++)
			free(themes[i].inherits[j]);
		free(themes[i].inherits);
		free(themes[i].name);
	}
	free(themes);
	themes = NULL;
	nthemes = tcap = 0;

	for (size_t i = 0; i < nchain; i++)
		free(chain[i]);
	free(chain);
	chain = NULL;
	nchain = 0;
	chain_built = false;

	for (size_t i = 0; i < nbase_dirs; i++)
		free(base_dirs[i]);
	free(base_dirs);
	base_dirs = NULL;
	nbase_dirs = bcap = 0;
}
