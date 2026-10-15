#include <climits>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <glib.h>
#include <librsvg/rsvg.h>
#include "icons.h"

// Only these directory kinds matter here; per-theme data and the loaded
// surfaces are cached for the process lifetime and dropped by clear().

namespace {

constexpr char ascii_lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c;
}

constexpr bool ci_contains(std::string_view s, std::string_view needle) {
	if (needle.empty() || s.size() < needle.size())
		return false;
	for (size_t i = 0; i + needle.size() <= s.size(); i++) {
		size_t j = 0;
		while (j < needle.size() && ascii_lower(s[i + j]) == ascii_lower(needle[j]))
			j++;
		if (j == needle.size())
			return true;
	}
	return false;
}

constexpr bool ci_equal(std::string_view a, std::string_view b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (ascii_lower(a[i]) != ascii_lower(b[i]))
			return false;
	return true;
}

std::string path_join(std::string_view a, std::string_view b) {
	while (!a.empty() && a.back() == '/')
		a.remove_suffix(1);
	if (!b.empty() && b.front() == '/')
		b.remove_prefix(1);
	std::string s;
	s.reserve(a.size() + b.size() + 1);
	s += a;
	s += '/';
	s += b;
	return s;
}

// Strips surrounding whitespace and quotes (gsettings output, settings.ini values).
void strip_quotes(std::string &s) {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '
			|| s.back() == '\t' || s.back() == '\'' || s.back() == '"'))
		s.pop_back();
	size_t off = 0;
	while (off < s.size() && (s[off] == ' ' || s[off] == '\t' || s[off] == '\''
			|| s[off] == '"'))
		off++;
	s.erase(0, off);
}

// ---- theme detection (gsettings, then GTK settings.ini) ----

std::optional<std::string> theme_from_ini(const std::string &path) {
	Handle<FILE, fclose> f{fopen(path.c_str(), "r")};
	if (!f)
		return std::nullopt;
	constexpr std::string_view key = "gtk-icon-theme-name";
	char line[512];
	while (fgets(line, sizeof(line), f.get())) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, key.data(), key.size()) != 0)
			continue;
		s += key.size();
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s != '=')
			continue;
		s++;
		while (*s == ' ' || *s == '\t')
			s++;
		std::string val = s;
		strip_quotes(val);
		if (!val.empty())
			return val;
	}
	return std::nullopt;
}

std::optional<std::string> detect_theme() {
	// pclose's status decides whether the output counts, so it runs inline
	if (FILE *f = popen("gsettings get org.gnome.desktop.interface icon-theme", "r")) {
		char buf[256] = "";
		bool got = fgets(buf, sizeof(buf), f) != nullptr;
		int status = pclose(f);
		if (got && status == 0) {
			std::string theme = buf;
			strip_quotes(theme);
			if (!theme.empty())
				return theme;
		}
	}

	std::optional<std::string> base;
	if (const char *x = getenv("XDG_CONFIG_HOME"); x && *x) {
		base = x;
	} else if (const char *home = getenv("HOME")) {
		base = path_join(home, ".config");
	}
	std::optional<std::string> theme;
	if (base) {
		constexpr std::string_view vers[2] = {"gtk-4.0", "gtk-3.0"};
		for (size_t i = 0; i < 2 && !theme; i++)
			theme = theme_from_ini(path_join(path_join(*base, vers[i]), "settings.ini"));
	}
	return theme;
}

// ---- index.theme parsing ----

int kf_int(GKeyFile *kf, const char *group, const char *key, int def) {
	GError *raw = nullptr;
	gint v = g_key_file_get_integer(kf, group, key, &raw);
	Handle<GError, g_error_free> err{raw};
	if (raw)
		return def;
	return static_cast<int>(v);
}

// ---- loading ----

// The image scaled to fit `size` x `size` (aspect kept, centred).
CairoSurfacePtr scale_to_fit(cairo_surface_t *img, int size) {
	int w = cairo_image_surface_get_width(img);
	int h = cairo_image_surface_get_height(img);
	if (w <= 0 || h <= 0)
		return nullptr;
	double s = static_cast<double>(size) / static_cast<double>(w > h ? w : h);
	CairoSurfacePtr out{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size)};
	if (cairo_surface_status(out.get()) != CAIRO_STATUS_SUCCESS)
		return nullptr;
	CairoPtr cr{cairo_create(out.get())};
	cairo_scale(cr.get(), s, s);
	cairo_set_source_surface(cr.get(), img, (size / s - w) / 2.0, (size / s - h) / 2.0);
	cairo_pattern_set_filter(cairo_get_source(cr.get()), CAIRO_FILTER_GOOD);
	cairo_paint(cr.get());
	return out;
}

CairoSurfacePtr load_png(std::string_view path, int size) {
	CairoSurfacePtr img{cairo_image_surface_create_from_png(std::string(path).c_str())};
	if (cairo_surface_status(img.get()) != CAIRO_STATUS_SUCCESS)
		return nullptr;
	return scale_to_fit(img.get(), size);
}

CairoSurfacePtr load_svg(std::string_view path, int size) {
	GError *raw = nullptr;
	Handle<RsvgHandle, g_object_unref> handle{rsvg_handle_new_from_file(
		std::string(path).c_str(), &raw)};
	Handle<GError, g_error_free> err{raw};
	if (!handle)
		return nullptr;
	CairoSurfacePtr out{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size)};
	if (cairo_surface_status(out.get()) != CAIRO_STATUS_SUCCESS)
		return nullptr;
	CairoPtr cr{cairo_create(out.get())};
	RsvgRectangle viewport{};
	viewport.width = size;
	viewport.height = size;
	GError *render_err = nullptr;
	bool ok = rsvg_handle_render_document(handle.get(), cr.get(), &viewport, &render_err);
	Handle<GError, g_error_free> render_guard{render_err};
	if (!ok)
		return nullptr;
	return out;
}

CairoSurfacePtr load_file_icon(std::string_view path, int size) {
	size_t dot = path.rfind('.');
	if (dot != std::string_view::npos) {
		std::string_view suffix = path.substr(dot);
		if (ci_equal(suffix, ".svg"))
			return load_svg(path, size);
		if (ci_equal(suffix, ".png"))
			return load_png(path, size);
	}
	if (CairoSurfacePtr s = load_png(path, size))
		return s;
	return load_svg(path, size);
}

} // namespace

Icons::Icons() = default;
Icons::~Icons() = default;

// ---- base dirs ----

void Icons::add_base_dir(std::string_view path) {
	if (path.empty())
		return;
	std::string dir = path_join(path, "icons");
	for (const std::string &b : base_dirs_)
		if (b == dir)
			return;
	base_dirs_.push_back(std::move(dir));
}

void Icons::ensure_base_dirs() {
	if (!base_dirs_.empty())
		return;
	const char *home = getenv("HOME");

	if (const char *x = getenv("XDG_DATA_HOME"); x && *x) {
		add_base_dir(x);
	} else if (home) {
		add_base_dir(path_join(home, ".local/share"));
	}
	if (home)
		add_base_dir(path_join(home, ".icons"));

	const char *dd = getenv("XDG_DATA_DIRS");
	if (!dd || !*dd)
		dd = "/usr/local/share:/usr/share";
	for (std::string_view p = dd; !p.empty();) {
		size_t colon = p.find(':');
		std::string_view base = p.substr(0, colon);
		if (!base.empty())
			add_base_dir(base);
		if (colon == std::string_view::npos)
			break;
		p.remove_prefix(colon + 1);
	}
}

void Icons::add_icon_dir(Theme &th, GKeyFile *kf, const char *dir) {
	GError *raw = nullptr;
	gint size = g_key_file_get_integer(kf, dir, "Size", &raw);
	Handle<GError, g_error_free> err{raw};
	if (raw)
		return; // Size is required by the spec

	Dir d;
	d.size = static_cast<int>(size);
	d.scale = kf_int(kf, dir, "Scale", 1);
	if (d.scale < 1)
		d.scale = 1;
	d.min_size = kf_int(kf, dir, "MinSize", d.size);
	d.max_size = kf_int(kf, dir, "MaxSize", d.size);
	d.threshold = kf_int(kf, dir, "Threshold", 2);
	if (d.threshold < 0)
		d.threshold = 0;

	Handle<gchar, g_free> type{g_key_file_get_string(kf, dir, "Type", nullptr)};
	if (type && ci_equal(type.get(), "Fixed"))
		d.type = DirType::fixed;
	else if (type && ci_equal(type.get(), "Scalable"))
		d.type = DirType::scalable;
	else
		d.type = DirType::threshold;

	Handle<gchar, g_free> ctx{g_key_file_get_string(kf, dir, "Context", nullptr)};
	d.apps = (ctx && ci_equal(ctx.get(), "Applications"))
		|| ci_contains(dir, "apps");

	d.name = dir;
	th.dirs.push_back(std::move(d));
}

void Icons::add_inherit(Theme &th, std::string_view name) {
	if (name.empty())
		return;
	th.inherits.emplace_back(name);
}

void Icons::load_theme_index(Theme &th) {
	ensure_base_dirs();
	for (const std::string &base : base_dirs_) {
		std::string idx = path_join(path_join(base, th.name), "index.theme");
		Handle<GKeyFile, g_key_file_free> kf{g_key_file_new()};
		// index.theme lists (Inherits, Directories) are comma-separated.
		g_key_file_set_list_separator(kf.get(), ',');
		GError *raw = nullptr;
		if (g_key_file_load_from_file(kf.get(), idx.c_str(), G_KEY_FILE_NONE, &raw)) {
			Handle<GError, g_error_free> err{raw};
			if (gchar **inh = g_key_file_get_string_list(kf.get(), "Icon Theme",
					"Inherits", nullptr, nullptr)) {
				for (gchar **i = inh; *i; i++)
					add_inherit(th, *i);
				g_strfreev(inh);
			}
			if (gchar **dirs = g_key_file_get_string_list(kf.get(), "Icon Theme",
					"Directories", nullptr, nullptr)) {
				for (gchar **i = dirs; *i; i++)
					add_icon_dir(th, kf.get(), *i);
				g_strfreev(dirs);
			}
			if (gchar **sdirs = g_key_file_get_string_list(kf.get(), "Icon Theme",
					"ScaledDirectories", nullptr, nullptr)) {
				for (gchar **i = sdirs; *i; i++)
					add_icon_dir(th, kf.get(), *i);
				g_strfreev(sdirs);
			}
			return; // first index.theme found wins
		}
		Handle<GError, g_error_free> err{raw};
	}
}

Icons::Theme &Icons::theme_get(std::string_view name) {
	for (Theme &th : themes_)
		if (th.name == name)
			return th;
	themes_.emplace_back();
	themes_.back().name = std::string(name);
	load_theme_index(themes_.back());
	return themes_.back();
}

// ---- theme chain ----

void Icons::chain_push(std::vector<std::string> &chain, std::string_view name) {
	for (const std::string &c : chain)
		if (std::string_view(c) == name)
			return;
	if (chain.size() >= max_chain)
		return;
	chain.emplace_back(name);
}

void Icons::build_chain() {
	if (chain_built_)
		return;
	chain_built_ = true;
	if (std::optional<std::string> t = detect_theme())
		chain_push(chain_, *t);
	for (size_t i = 0; i < chain_.size() && chain_.size() < max_chain; i++) {
		Theme &th = theme_get(chain_[i]);
		for (size_t j = 0; j < th.inherits.size() && chain_.size() < max_chain; j++)
			chain_push(chain_, th.inherits[j]);
	}
	chain_push(chain_, "hicolor");
}

// ---- lookup ----

// 0 when the directory covers `want` (exactly, Scalable range or within the
// threshold); otherwise the distance to the nearest edge.
int Icons::dir_distance(const Dir &d, int want) {
	int base = d.size * d.scale;
	int lo = base, hi = base;
	if (d.type == DirType::scalable) {
		lo = d.min_size * d.scale;
		hi = d.max_size * d.scale;
	} else if (d.type == DirType::threshold) {
		lo = base - d.threshold * d.scale;
		hi = base + d.threshold * d.scale;
	}
	if (want >= lo && want <= hi)
		return 0;
	return want < lo ? lo - want : want - hi;
}

// First existing <base>/<theme>/<dir>/<name><ext> across the base dirs.
std::optional<std::string> Icons::find_in_dir(std::string_view theme, const Dir &dir,
	std::string_view fname) const {
	for (int e = 0; e < 2; e++) {
		const char *ext = e == 0 ? ".png" : ".svg";
		std::string full = std::string(fname) + ext;
		for (const std::string &base : base_dirs_) {
			std::string cand = path_join(path_join(path_join(base, theme), dir.name), full);
			if (access(cand.c_str(), R_OK) == 0)
				return cand;
		}
	}
	return std::nullopt;
}

CairoSurfacePtr Icons::find_theme_icon(std::string_view name, int size) {
	build_chain();
	for (int pass = 0; pass < 2; pass++) {
		bool apps_only = pass == 0;
		for (size_t t = 0; t < chain_.size(); t++) {
			Theme &th = theme_get(chain_[t]);
			std::optional<std::string> best;
			int best_dist = INT_MAX;
			for (const Dir &d : th.dirs) {
				if (d.apps != apps_only)
					continue;
				int dist = dir_distance(d, size);
				if (dist > best_dist)
					continue;
				std::optional<std::string> cand = find_in_dir(chain_[t], d, name);
				if (!cand)
					continue;
				if (dist < best_dist) {
					best = std::move(cand);
					best_dist = dist;
				}
			}
			if (best) {
				CairoSurfacePtr s = load_file_icon(*best, size);
				return s; // first theme with any hit wins
			}
		}
	}
	return nullptr;
}

CairoSurfacePtr Icons::load_icon(std::string_view icon, int size) {
	if (!icon.empty() && icon.front() == '/')
		return load_file_icon(icon, size);

	// theme icon: drop a known extension from the name
	std::string_view name = icon;
	std::string name_buf;
	size_t dot = icon.rfind('.');
	if (dot != std::string_view::npos) {
		std::string_view suffix = icon.substr(dot);
		if (ci_equal(suffix, ".png") || ci_equal(suffix, ".svg")
				|| ci_equal(suffix, ".xpm")) {
			size_t n = std::min(dot, size_t{511});
			name_buf = icon.substr(0, n);
			if (name_buf.empty())
				return nullptr;
			name = name_buf;
		}
	}

	if (CairoSurfacePtr s = find_theme_icon(name, size))
		return s;

	// last resort: /usr/share/pixmaps
	for (int e = 0; e < 2; e++) {
		const char *ext = e == 0 ? ".png" : ".svg";
		std::string path = path_join("/usr/share/pixmaps", std::string(name) + ext);
		if (access(path.c_str(), R_OK) == 0)
			if (CairoSurfacePtr s = load_file_icon(path, size))
				return s;
	}
	return nullptr;
}

cairo_surface_t *Icons::lookup(std::string_view icon, int size) {
	if (icon.empty() || size <= 0)
		return nullptr;
	for (const Entry &c : cache_)
		if (c.size == size && std::string_view(c.icon) == icon)
			return c.surface.get();

	CairoSurfacePtr surface = load_icon(icon, size);
	cache_.push_back(Entry{std::string(icon), size, std::move(surface)});
	return cache_.back().surface.get();
}

void Icons::clear() {
	cache_.clear();
	themes_.clear();
	chain_.clear();
	chain_built_ = false;
	base_dirs_.clear();
}
