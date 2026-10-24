#include "canvas/overview_chrome.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "core/config.h"
#include "core/server.h"
#include "platform/output.h"
#include "render/draw.h"
#include "render/pass.h"
#include "render/text_cache.h"

namespace {

using LabelBuffer = std::array<char, 48>;

// "Desktop 3", with the percentage of a scaled desktop.
std::string_view desktop_label_text(const Desktop &desktop, LabelBuffer &buf) {
	int len;
	if (desktop.scale() == 1.0) {
		len = std::snprintf(buf.data(), buf.size(), "Desktop %d", desktop.number());
	} else {
		len = std::snprintf(buf.data(), buf.size(), "Desktop %d · %d %%",
			desktop.number(), static_cast<int>(std::lround(desktop.scale() * 100)));
	}
	return {buf.data(), std::min(static_cast<std::size_t>(std::max(len, 0)), buf.size() - 1)};
}

// The tab's label: the text cache's request for its text, in `fg`.
TextSpec desktop_label_spec(std::string_view text, float scale, uint32_t fg) {
	return {.text = text, .font = config::ui_font_bold, .fg = fg, .scale = scale};
}

// The Tile switch's glyph: four squares centred in the button.
void draw_tile_glyph(const RenderCtx &ctx, const wlr_box &button, wlr_render_color color) {
	constexpr int square = 7, gap = 2;
	int x0 = button.x + (button.width - (2 * square + gap)) / 2;
	int y0 = button.y + (button.height - (2 * square + gap)) / 2;
	for (int row = 0; row < 2; row++) {
		for (int col = 0; col < 2; col++) {
			draw_logical_rect(ctx, x0 + col * (square + gap), y0 + row * (square + gap),
				square, square, color);
		}
	}
}

// The Abandon button's glyph: a cross centred in the button.
void draw_abandon_glyph(const RenderCtx &ctx, const wlr_box &button, wlr_render_color color) {
	constexpr int size = 10;
	int x0 = button.x + (button.width - size) / 2;
	int y0 = button.y + (button.height - size) / 2;
	draw_stepped_line(ctx, x0, y0, x0 + size, y0 + size, 2.0, 1.0, color);
	draw_stepped_line(ctx, x0, y0 + size, x0 + size, y0, 2.0, 1.0, color);
}

void draw_tabs(const RenderCtx &ctx) {
	Output &o = *ctx.output;
	Server &server = o.server;
	for (const Desktop &d : server.canvas.desktops.all()) {
		// The target's tab is the accent fill, so its label is drawn in
		// the accent-text variant: a contrasting colour in both modes.
		bool target = o.overview.target().desktop == d.id();
		CachedText label = desktop_label(server, d, ctx.scale, target);
		std::optional<TabLayout> layout = overview_tab_layout(o, d);
		if (!layout) {
			continue;
		}
		uint32_t bg = target ? server.theme.colors().accent : server.theme.colors().panel;
		draw_logical_rect(ctx, layout->tab.x, layout->tab.y, layout->tab.width,
			layout->tab.height, color_rgba(bg));
		if (label.texture != nullptr && label.w > 0 && label.h > 0) {
			draw_ui_texture(ctx, label.texture, layout->tab.x + config::tab_pad,
				layout->tab.y + (layout->tab.height - label.h) / 2,
				label.w, label.h, 1.0);
		}
		uint32_t ink = target ? server.theme.colors().accent_text
			: server.theme.colors().panel_text;
		uint32_t lit = target ? server.theme.colors().accent_text
			: server.theme.colors().accent;
		wlr_render_color fg = color_rgba(ink);
		if (d.tiling()) {
			draw_logical_rect(ctx, layout->tile_button.x, layout->tile_button.y,
				layout->tile_button.width, layout->tile_button.height,
				color_rgba(lit, 0.25f));
			fg = color_rgba(lit);
		}
		draw_tile_glyph(ctx, layout->tile_button, fg);
		draw_abandon_glyph(ctx, layout->abandon_button, color_rgba(ink));
	}
}

// The y the overview's bottom UI — the help line, the hint's row, the shelf —
// stays above: the output's bottom edge, or the top of the highest shell
// strip (the toolbar, the app bar) anchored to it.
int strips_top(const Output &o) {
	int bottom = o.height;
	for (const auto &vec : o.layers) {
		for (const std::unique_ptr<LayerSurface> &layer : vec) {
			if (layer->mapped && layer->is_overview_strip()
					&& (layer->wlr->current.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0) {
				bottom = std::min(bottom, layer->geo.y);
			}
		}
	}
	return bottom;
}

// Shelf thumbnails: the view's frame fitted into the box, aspect kept.
void draw_shelf(const RenderCtx &ctx) {
	Shelf shelf = overview_shelf_layout(*ctx.output);
	if (shelf.empty()) {
		return;
	}
	int left = shelf.front().box.x, right = shelf.back().box.x + shelf.back().box.width;
	int top = shelf.front().box.y, bottom = shelf.front().box.y + shelf.front().box.height;
	draw_logical_rect(ctx, left - config::shelf_backing, top - config::shelf_backing,
		right - left + 2 * config::shelf_backing, bottom - top + 2 * config::shelf_backing,
		color_rgba(ctx.output->server.theme.colors().panel));

	for (std::size_t i = 0; i < shelf.size(); i++) {
		View *view = shelf[i].view;
		const wlr_box &box = shelf[i].box;
		wlr_box frame = view->frame_box();
		double fit = 0.0;
		if (frame.width > 0 && frame.height > 0) {
			fit = std::fmin(box.width / static_cast<double>(frame.width),
				(box.height - config::shelf_title_strip) / frame.height);
		}
		Point origin = view->surface_origin();
		double ox = origin.x, oy = origin.y;
		view->for_each_surface([&](wlr_surface *surface, int sx, int sy) {
			double cx = ox + sx, cy = oy + sy;
			double cw = surface->current.width, ch = surface->current.height;
			// Popups sticking out of the frame cannot be cropped (the source box
			// would have to move too): leave them out of the thumbnail.
			if (cx < frame.x || cy < frame.y
					|| cx + cw > frame.x + frame.width
					|| cy + ch > frame.y + frame.height) {
				return;
			}
			double thumbs_h = box.height - config::shelf_title_strip;
			double x = box.x + (box.width - frame.width * fit) / 2.0
				+ (cx - frame.x) * fit;
			double y = box.y + (thumbs_h - frame.height * fit) / 2.0
				+ (cy - frame.y) * fit;
			draw_surface_logical(ctx, surface, x, y, cw * fit, ch * fit, 1.0f, true);
		});

		const char *title = view->shell_title();
		if (title != nullptr && title[0] != '\0') {
			Server &server = ctx.output->server;
			CachedText text = server.text_cache.get({.text = title, .font = config::ui_font,
				.fg = server.theme.colors().panel_text, .scale = ctx.scale});
			if (text.texture != nullptr && text.w > 0 && text.h > 0) {
				double w = std::fmin(text.w, static_cast<double>(box.width));
				draw_ui_texture(ctx, text.texture, box.x + (box.width - w) / 2.0,
					box.y + box.height - text.h, w, text.h, w / text.w);
			}
		}
	}
}

void draw_help_line(const RenderCtx &ctx, const OverviewBottom &bottom) {
	Server &server = ctx.output->server;
	CachedText help = server.text_cache.get(panel_text_spec(
		overview_help_text(server.gestures.options()), ctx.scale, server.theme.colors()));
	if (help.texture == nullptr || wlr_box_empty(&bottom.help)) {
		return;
	}
	draw_ui_texture(ctx, help.texture, bottom.help.x, bottom.help.y,
		bottom.help.width, bottom.help.height, 1.0);
}

} // namespace

const char *overview_help_text(const GestureOptions &options) {
	switch (options.swipes) {
	case settings::SwipePreset::three_finger_hops:
		return "3 fingers: jump to desktop  ·  4 down: go there  ·  "
			"4 sideways: abandon  ·  3-finger pinch out / in: tile / combine  ·  "
			"drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	case settings::SwipePreset::four_finger_hops:
		return "4 fingers: jump to desktop  ·  3 down: go there  ·  "
			"3 sideways: abandon  ·  4-finger pinch out / in: tile / combine  ·  "
			"drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	case settings::SwipePreset::off:
		return "drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	}
	return "";
}

CachedText desktop_label(Server &server, const Desktop &desktop, float scale, bool on_accent) {
	LabelBuffer buf;
	const config::Palette &pal = server.theme.colors();
	return server.text_cache.get(desktop_label_spec(desktop_label_text(desktop, buf), scale,
		on_accent ? pal.accent_text : pal.panel_text));
}

std::optional<TabLayout> overview_tab_layout(const Output &o, const Desktop &desktop) {
	Server &server = o.server;
	Camera cam = o.camera.drawn();
	double left = cam.cx - o.width / (2.0 * cam.zoom);
	double top = cam.cy - o.height / (2.0 * cam.zoom);
	double offset = server.gestures.abandon_offset(desktop.id());
	double frame_left = (desktop.frame().x - left) * cam.zoom + offset;
	double frame_top = (desktop.frame().y - top) * cam.zoom;

	// The label's width once the text cache has drawn it; until then a guess
	// of config::tab_label_guess per character.
	LabelBuffer buf;
	std::string_view text = desktop_label_text(desktop, buf);
	std::optional<CachedText> label = server.text_cache.find_size(
		desktop_label_spec(text, o.wlr->scale, 0));
	int text_w = label.has_value() ? label->w : config::tab_label_guess * static_cast<int>(text.size());

	TabLayout layout;
	layout.tab.x = static_cast<int>(std::round(frame_left));
	int frame_y = static_cast<int>(std::round(frame_top));
	layout.tab.y = frame_y - config::tab_height - config::tab_lift;
	if (layout.tab.y < config::tab_lift) {
		layout.tab.y = frame_y + config::tab_lift;
	}
	layout.tab.width = config::tab_pad + text_w + config::tab_gap
		+ 2 * config::tab_button + config::tab_button_gap;
	layout.tab.height = config::tab_height;

	layout.tile_button = {layout.tab.x + config::tab_pad + text_w + config::tab_gap,
		layout.tab.y + config::tab_button_inset, config::tab_button, config::tab_button};
	layout.abandon_button = {layout.tile_button.x + config::tab_button + config::tab_button_gap,
		layout.tile_button.y, config::tab_button, config::tab_button};

	if (layout.tab.x < o.width && layout.tab.x + layout.tab.width > 0
			&& layout.tab.y < o.height && layout.tab.y + layout.tab.height > 0) {
		return layout;
	}
	return std::nullopt;
}

OverviewBottom overview_bottom_layout(const Output &o, float scale) {
	Server &server = o.server;
	CachedText help = server.text_cache.get(panel_text_spec(
		overview_help_text(server.gestures.options()), scale, server.theme.colors()));
	bool has_help = help.texture != nullptr && help.w > 0 && help.h > 0;
	OverviewBottom stack;
	int help_top = strips_top(o) - config::overview_help_margin;
	if (has_help) {
		// Narrow outputs: shrink the line rather than clip it.
		double fit = std::fmin(1.0, (o.width - 2.0 * config::overview_help_margin) / help.w);
		int w = static_cast<int>(std::ceil(help.w * fit));
		int h = static_cast<int>(std::ceil(help.h * fit));
		help_top -= h;
		stack.help = {(o.width - w) / 2, help_top, w, h};
	}
	// A hint's texture is as tall as the help line's: one font, one padding.
	int line_h = has_help ? help.h : config::overview_help_guess_h;
	stack.hint = {0, help_top - config::overview_hint_gap - line_h, o.width, line_h};
	stack.shelf_bottom = stack.hint.y - config::overview_shelf_gap;
	return stack;
}

Shelf overview_shelf_layout(const Output &o) {
	Shelf shelf;
	for (View *view : o.server.views) {
		if (!view->mapped || !view->minimized || shelf.size() == shelf_max) {
			continue;
		}
		// Oldest minimized first: minimize_seq ascending.
		shelf.push_back({});
		std::size_t i = shelf.size() - 1;
		while (i > 0 && shelf[i - 1].view->minimize_seq > view->minimize_seq) {
			shelf[i] = shelf[i - 1];
			i--;
		}
		shelf[i] = {.view = view, .box = {}};
	}
	int count = static_cast<int>(shelf.size());
	int total = count * config::shelf_thumb_w + (count - 1) * config::shelf_gap;
	int x = (o.width - total) / 2;
	int bottom = overview_bottom_layout(o, o.wlr->scale).shelf_bottom;
	for (int i = 0; i < count; i++) {
		shelf[static_cast<std::size_t>(i)].box = {
			.x = x + i * (config::shelf_thumb_w + config::shelf_gap),
			.y = bottom - config::shelf_thumb_h,
			.width = config::shelf_thumb_w,
			.height = config::shelf_thumb_h};
	}
	return shelf;
}

void draw_overview_chrome(const RenderCtx &ctx, const OverviewBottom &bottom) {
	draw_tabs(ctx);
	draw_shelf(ctx);
	draw_help_line(ctx, bottom);
}
