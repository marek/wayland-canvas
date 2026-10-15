#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <cairo.h>
#include "util.h"

// glib's own declaration: the header stays free of glib for everything including launcher.h.
using GKeyFile = struct _GKeyFile;

// freedesktop icon theme lookup.
class Icons {
public:
	Icons();
	~Icons();
	Icons(const Icons &) = delete;
	Icons &operator=(const Icons &) = delete;

	// `icon` is an absolute path or a theme icon name; `size` in device pixels. Looks in the
	// user's icon theme (gsettings/GTK settings), its Inherits chain, hicolor, then pixmaps;
	// loads PNG (cairo) or SVG (librsvg) scaled to fit size x size. Cached per (icon, size):
	// the cache owns the surface. nullptr if not found.
	cairo_surface_t *lookup(std::string_view icon, int size);

	// Drops every cached surface and the theme index.
	void clear();

private:
	enum class DirType { fixed, scalable, threshold };

	struct Dir {
		std::string name; // path inside the theme, e.g. "48x48/apps"
		int size = 0;
		int scale = 1;
		int min_size = 0;
		int max_size = 0;
		int threshold = 2;
		DirType type = DirType::threshold;
		bool apps = false; // Context=Applications (or "apps" in the path): searched first
	};

	struct Theme {
		std::string name;
		std::vector<Dir> dirs;
		std::vector<std::string> inherits;
	};

	// One cached (icon, size) lookup; a null surface means looked up and not found.
	struct Entry {
		std::string icon;
		int size = 0;
		CairoSurfacePtr surface;
	};

	void add_base_dir(std::string_view path);
	void ensure_base_dirs();
	void add_icon_dir(Theme &th, GKeyFile *kf, const char *dir);
	static void add_inherit(Theme &th, std::string_view name);
	void load_theme_index(Theme &th);
	Theme &theme_get(std::string_view name);
	static void chain_push(std::vector<std::string> &chain, std::string_view name);
	void build_chain();
	static int dir_distance(const Dir &d, int want);
	std::optional<std::string> find_in_dir(std::string_view theme, const Dir &dir,
		std::string_view fname) const;
	CairoSurfacePtr find_theme_icon(std::string_view name, int size);
	CairoSurfacePtr load_icon(std::string_view icon, int size);

	std::vector<Theme> themes_;
	std::vector<std::string> chain_; // theme names: user theme, its Inherits..., hicolor
	bool chain_built_ = false;
	std::vector<std::string> base_dirs_;
	std::vector<Entry> cache_;

	static constexpr size_t max_chain = 8;
};
