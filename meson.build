project(
	'wayland-canvas',
	'cpp',
	'c',
	version: '0.1.0',
	license: 'Apache-2.0',
	meson_version: '>=1.3.0',
	default_options: [
		'cpp_std=c++26',
		'cpp_eh=none',
		'cpp_rtti=false',
		'warning_level=3',
		'buildtype=debugoptimized',
	],
)

foreach lang : ['cpp', 'c']
	if meson.get_compiler(lang).get_id() != 'clang'
		error('wayland-canvas builds with LLVM: meson setup --wipe build --native-file toolchain/llvm.ini')
	endif
endforeach

add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',
	language: 'cpp',
)

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
	include_type: 'system',
)
wayland_server = dependency('wayland-server', include_type: 'system')
wayland_scanner = dependency('wayland-scanner', native: true)
wayland_scanner_prog = find_program(
	wayland_scanner.get_variable('wayland_scanner'),
	native: true,
)
xkbcommon = dependency('xkbcommon', include_type: 'system')
pixman = dependency('pixman-1', include_type: 'system')
cairo = dependency('cairo', include_type: 'system')
pangocairo = dependency('pangocairo', include_type: 'system')
libinput = dependency('libinput', include_type: 'system')
libdrm = dependency('libdrm', include_type: 'system').partial_dependency(compile_args: true, includes: true)
librsvg = dependency('librsvg-2.0', include_type: 'system')
glib = dependency('glib-2.0', include_type: 'system')
gio = dependency('gio-2.0', include_type: 'system')
json_glib = dependency('json-glib-1.0', include_type: 'system')
sqlite3 = dependency('sqlite3', include_type: 'system')

protocol_headers = []
foreach xml : ['protocols/wlr-layer-shell-unstable-v1.xml']
	protocol_headers += custom_target(
		xml.underscorify() + '_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

# Includes are written from src/ ("canvas/desktop.h").
src_inc = include_directories('src')

# Pure logic without wlroots, unit-tested on its own.
geometry_lib = static_library(
	'canvas-geometry',
	'src/core/geometry.cpp',
	include_directories: src_inc,
)
desktop_entry_lib = static_library(
	'canvas-desktop-entry',
	'src/apps/desktop_entry.cpp',
	include_directories: src_inc,
)

theme_prefs_lib = static_library(
	'canvas-theme-prefs',
	'src/core/theme_prefs.cpp',
	include_directories: src_inc,
	dependencies: [glib, gio],
)

session_bus_lib = static_library(
	'canvas-session-bus',
	'src/platform/session_bus.cpp',
	include_directories: src_inc,
)

# The AI usage collector the agents panel's widget runs: pure logic over
# the tools' own records, unit-tested on its own, and a thin helper
# executable installed beside the compositor for the widget to run by
# name.
agent_usage_lib = static_library(
	'canvas-agent-usage',
	'src/shell/agent_usage.cpp',
	include_directories: src_inc,
	dependencies: [glib, json_glib, sqlite3],
)

agent_usage = executable(
	'wayland-canvas-agent-usage',
	'src/shell/agent_usage_main.cpp',
	include_directories: src_inc,
	link_with: agent_usage_lib,
	dependencies: [glib, json_glib, sqlite3],
	install: true,
)

core_lib = static_library(
	'canvas-core',
	files(
		'src/apps/apps.cpp',
		'src/apps/icons.cpp',
		'src/apps/launcher.cpp',
		'src/canvas/camera.cpp',
		'src/canvas/canvas.cpp',
		'src/canvas/items.cpp',
		'src/canvas/desktop.cpp',
		'src/canvas/overview.cpp',
		'src/core/server.cpp',
		'src/input/bindings.cpp',
		'src/input/gesture.cpp',
		'src/input/grab.cpp',
		'src/input/input.cpp',
		'src/platform/ipc.cpp',
		'src/platform/layer.cpp',
		'src/platform/output.cpp',
		'src/render/draw.cpp',
		'src/render/render.cpp',
		'src/windows/decoration.cpp',
		'src/windows/view.cpp',
	),
	protocol_headers,
	include_directories: src_inc,
	link_with: [geometry_lib, desktop_entry_lib, theme_prefs_lib, session_bus_lib],
	dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, pangocairo, librsvg,
		glib, json_glib, libinput, libdrm],
)

wayland_canvas = executable(
	'wayland-canvas',
	'src/main.cpp',
	protocol_headers,
	include_directories: src_inc,
	link_with: core_lib,
	dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, libinput, libdrm],
	install: true,
)

# The shell (the plugin host and its UI kit) and the built-in plugins, both
# found beside the binary in the build tree, and under
# <prefix>/share/wayland-canvas once installed.
install_subdir(
	'shell',
	install_dir: get_option('datadir') / 'wayland-canvas',
)
install_subdir(
	'plugins',
	install_dir: get_option('datadir') / 'wayland-canvas',
)

# What the distribution packages carry beyond the binary: the session file
# the display managers list, the default-session tool, and the portals
# config that sends screen sharing to the wlroots backend.
install_data(
	'packaging/wayland-canvas.desktop',
	install_dir: get_option('datadir') / 'wayland-sessions',
)
install_data(
	'packaging/wayland-canvas-portals.conf',
	install_dir: get_option('datadir') / 'xdg-desktop-portal',
)
install_data(
	'packaging/wayland-canvas-session',
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)

# ninja -C build package-arch / package-deb — artifacts under build/packages/.
run_target(
	'package-arch',
	command: [find_program('tools/package-arch.sh'), meson.project_build_root()],
)
run_target(
	'package-deb',
	command: [find_program('tools/package-deb.sh'), meson.project_build_root()],
)

test(
	'geometry',
	executable(
		'test-geometry',
		'tests/test_geometry.cpp',
		include_directories: src_inc,
		link_with: geometry_lib,
	),
	suite: 'unit',
)

test(
	'desktop-entry',
	executable(
		'test-desktop-entry',
		'tests/test_desktop_entry.cpp',
		include_directories: src_inc,
		link_with: desktop_entry_lib,
	),
	suite: 'unit',
)

theme_prefs_env = environment()
theme_prefs_env.set('CANVAS_TMP', meson.project_source_root() / 'tmp')
test(
	'theme-prefs',
	executable(
		'test-theme-prefs',
		'tests/test_theme_prefs.cpp',
		include_directories: src_inc,
		link_with: theme_prefs_lib,
		dependencies: [glib, gio],
	),
	env: theme_prefs_env,
	suite: 'unit',
)

test(
	'agent-usage',
	executable(
		'test-agent-usage',
		'tests/test_agent_usage.cpp',
		include_directories: src_inc,
		link_with: agent_usage_lib,
		dependencies: [glib, json_glib, sqlite3],
	),
	suite: 'unit',
)

test(
	'session-bus',
	executable(
		'test-session-bus',
		'tests/test_session_bus.cpp',
		include_directories: src_inc,
		link_with: session_bus_lib,
	),
	suite: 'unit',
)

# End-to-end scenarios: the real compositor on the headless backend, real
# foot clients, fake input devices, grim screenshots. Registered only when
# foot and grim are on PATH; the bar scenario additionally needs qs (the
# bar's own client), dbus-daemon and the dbusmock stand-in environment.
# They still need a GPU render node at run time (headless GLES2 rendering).
# Serial: each owns the display namespace.
foot_prog = find_program('foot', required: false)
grim_prog = find_program('grim', required: false)
if foot_prog.found() and grim_prog.found()
	scenario_env = environment()
	scenario_env.set('CANVAS_TMP', meson.project_source_root() / 'tmp')
	scenario_env.set('WAYLAND_CANVAS_BIN', wayland_canvas.full_path())
	scenarios = ['gestures', 'input', 'launcher', 'ipc']
	# The bar scenario drives the real bar client against stand-in services,
	# so it also needs qs, a dbus-daemon to build its private buses from,
	# and the dbusmock stand-in environment (provisioned under tmp/ by the
	# ensure script at configure time).
	qs_prog = find_program('qs', required: false)
	dbus_daemon_prog = find_program('dbus-daemon', required: false)
	bar_standins = run_command(
		'tests/scenarios/ensure-bar-standins.sh',
		check: false,
	)
	if qs_prog.found() and dbus_daemon_prog.found() and bar_standins.returncode() == 0
		scenarios += ['bar']
		scenario_env.set('CANVAS_BAR_STANDIN_PY', bar_standins.stdout().strip().split('\n')[-1])
		scenario_env.set(
			'CANVAS_BAR_STANDINS',
			meson.project_source_root() / 'tests/scenarios/bar-standins.py',
		)
		# The agents panel's widget runs the collector by name; the bar
		# scenario points its PATH at the build tree's copy.
		scenario_env.set('WAYLAND_CANVAS_AGENT_USAGE', agent_usage.full_path())
	endif
	foreach scenario : scenarios
		test(
			scenario,
			executable(
				'scenario-' + scenario,
				'tests/scenarios/' + scenario + '.cpp',
				protocol_headers,
				include_directories: src_inc,
				link_with: core_lib,
				dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, pangocairo,
					json_glib, libinput, libdrm],
			),
			suite: 'scenarios',
			is_parallel: false,
			# The bar scenario also walks every panel, the plugin reloads,
			# the idle watch, the theme phase, the Wi-Fi phase and the
			# agents phase (whose probe-reuse window alone waits out 20 s):
			# it needs the wider budget.
			timeout: scenario == 'bar' ? 420 : 180,
			env: scenario_env,
		)
	endforeach
endif

# The default-session tool against fake roots, one per display manager.
packaging_env = environment()
packaging_env.set('CANVAS_TMP', meson.project_source_root() / 'tmp')
test(
	'packaging',
	find_program('tests/test_packaging.sh'),
	suite: 'packaging',
	is_parallel: false,
	timeout: 120,
	env: packaging_env,
)
