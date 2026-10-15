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
	'src/geometry.cpp',
)
desktop_entry_lib = static_library(
	'canvas-desktop-entry',
	'src/desktop_entry.cpp',
)

core_lib = static_library(
	'canvas-core',
	files(
		'src/bindings.cpp',
		'src/apps.cpp',
		'src/camera.cpp',
		'src/canvas.cpp',
		'src/decoration.cpp',
		'src/desktop.cpp',
		'src/draw.cpp',
		'src/gesture.cpp',
		'src/grab.cpp',
		'src/icons.cpp',
		'src/input.cpp',
		'src/ipc.cpp',
		'src/launcher.cpp',
		'src/overview.cpp',
		'src/layer.cpp',
		'src/output.cpp',
		'src/render.cpp',
		'src/server.cpp',
		'src/view.cpp',
	),
	protocol_headers,
	link_with: [geometry_lib, desktop_entry_lib],
	dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, pangocairo, librsvg,
		glib, libinput, libdrm],
)

wayland_canvas = executable(
	'wayland-canvas',
	'src/main.cpp',
	protocol_headers,
	link_with: core_lib,
	dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, libinput, libdrm],
	install: true,
)

# The top bar's Quickshell config: found beside the binary in the build tree,
# and under <prefix>/share/wayland-canvas once installed.
install_subdir(
	'shell',
	install_dir: get_option('datadir') / 'wayland-canvas',
)

# What the distribution packages carry beyond the binary: the session file
# the display managers list, and the default-session tool.
install_data(
	'packaging/wayland-canvas.desktop',
	install_dir: get_option('datadir') / 'wayland-sessions',
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
		include_directories: include_directories('src'),
		link_with: geometry_lib,
	),
	suite: 'unit',
)

test(
	'desktop-entry',
	executable(
		'test-desktop-entry',
		'tests/test_desktop_entry.cpp',
		include_directories: include_directories('src'),
		link_with: desktop_entry_lib,
	),
	suite: 'unit',
)

# End-to-end scenarios: the real compositor on the headless backend, real
# foot clients, fake input devices, grim screenshots. Registered only when
# foot and grim are on PATH; they still need a GPU render node at run time
# (headless GLES2 rendering). Serial: each owns the display namespace.
foot_prog = find_program('foot', required: false)
grim_prog = find_program('grim', required: false)
if foot_prog.found() and grim_prog.found()
	scenario_env = environment()
	scenario_env.set('CANVAS_TMP', meson.project_source_root() / 'tmp')
	scenario_env.set('WAYLAND_CANVAS_BIN', wayland_canvas.full_path())
	foreach scenario : ['gestures', 'input', 'launcher', 'ipc']
		test(
			scenario,
			executable(
				'scenario-' + scenario,
				'tests/scenarios/' + scenario + '.cpp',
				protocol_headers,
				include_directories: include_directories('src'),
				link_with: core_lib,
				dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, pangocairo,
					libinput, libdrm],
			),
			suite: 'scenarios',
			is_parallel: false,
			timeout: 180,
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
