project(
	'wayland-canvas',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=1.3.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'buildtype=debugoptimized',
	],
)

add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-D_POSIX_C_SOURCE=200809L',
	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',
	language: 'c',
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m')

# Uses the system wlroots-0.20 when installed, otherwise builds the wrap in
# subprojects/ statically with only what this compositor needs.
wlroots = dependency(
	'wlroots-0.20',
	version: ['>=0.20.0', '<0.21.0'],
	fallback: 'wlroots',
	default_options: [
		'default_library=static',
		'examples=false',
		'renderers=gles2',
		'backends=drm,libinput',
		'xwayland=disabled',
		'session=enabled',
		'libliftoff=disabled',
	],
)
wayland_server = dependency('wayland-server')
wayland_scanner = dependency('wayland-scanner', native: true)
wayland_scanner_prog = find_program(
	wayland_scanner.get_variable('wayland_scanner'),
	native: true,
)
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
libinput = dependency('libinput')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)
librsvg = dependency('librsvg-2.0')
glib = dependency('glib-2.0')

protocol_headers = []
foreach xml : ['protocols/wlr-layer-shell-unstable-v1.xml']
	protocol_headers += custom_target(
		xml.underscorify() + '_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

# Pure logic without wlroots, unit-tested on its own.
geometry_lib = static_library(
	'canvas-geometry',
	'src/geometry.c',
	dependencies: m_dep,
)
desktop_entry_lib = static_library(
	'canvas-desktop-entry',
	'src/desktop_entry.c',
)

core_lib = static_library(
	'canvas-core',
	files(
		'src/apps.c',
		'src/canvas.c',
		'src/decoration.c',
		'src/draw.c',
		'src/gesture.c',
		'src/icons.c',
		'src/input.c',
		'src/launcher.c',
		'src/layer.c',
		'src/output.c',
		'src/render.c',
		'src/server.c',
		'src/view.c',
	),
	protocol_headers,
	link_with: [geometry_lib, desktop_entry_lib],
	dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, pangocairo, librsvg,
		glib, libinput, libdrm, m_dep],
)

executable(
	'wayland-canvas',
	'src/main.c',
	protocol_headers,
	link_with: core_lib,
	dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo],
	install: true,
)

test(
	'geometry',
	executable(
		'test-geometry',
		'tests/test_geometry.c',
		include_directories: include_directories('src'),
		link_with: geometry_lib,
		dependencies: m_dep,
	),
)

test(
	'desktop-entry',
	executable(
		'test-desktop-entry',
		'tests/test_desktop_entry.c',
		include_directories: include_directories('src'),
		link_with: desktop_entry_lib,
	),
)
