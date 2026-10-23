project(
	'wayland-canvas',
	'cpp',
	'c',
	# the git tag, or the VERSION file of a source tarball
	# (docs/intents/workspace/packaging.md)
	version: run_command('tools/version.sh', check: true).stdout().strip(),
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
		'xwayland=enabled',
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
# logind: `loginctl lock-session` and the sleep the lock is drawn before.
# Optional, so a machine without it still builds (it then has no triggers).
libsystemd = dependency('libsystemd', required: false, include_type: 'system')
add_project_arguments(
	'-DCANVAS_HAVE_LOGIND=' + (libsystemd.found() ? '1' : '0'),
	language: 'cpp',
)
# X11 applications through Xwayland: the X11 window manager is wlroots' own
# (its have_xwayland), and xcb is what this code needs on top of it.
if wlroots.get_variable(
	pkgconfig: 'have_xwayland',
	internal: 'have_xwayland',
) != 'true'
	error('wayland-canvas needs a wlroots built with Xwayland: install one that has it, or build the wlroots subproject with -Dwlroots:xwayland=enabled')
endif
xcb = dependency('xcb', include_type: 'system')

# What the compositor's core builds with, for it and everything linked to it.
core_deps = declare_dependency(
	dependencies: [wlroots, wayland_server, xkbcommon, pixman, cairo, pangocairo,
		librsvg, glib, gio, json_glib, libinput, libdrm, libsystemd, xcb],
)

protocol_headers = []
foreach xml : ['protocols/wlr-layer-shell-unstable-v1.xml']
	protocol_headers += custom_target(
		xml.underscorify() + '_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

# Client-side protocol glue for the small test clients below.
wayland_client = dependency('wayland-client', include_type: 'system')
wl_protocol_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
client_protocols = []
foreach xml : [
	wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
	wl_protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
	wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
	wl_protocol_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml',
	wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
	wl_protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
	wl_protocol_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
	wl_protocol_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
	'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
	'protocols/wlr-gamma-control-unstable-v1.xml',
	'protocols/wlr-layer-shell-unstable-v1.xml',
]
	client_protocols += custom_target(
		xml.underscorify() + '_client_h',
		input: xml,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'],
	)
	client_protocols += custom_target(
		xml.underscorify() + '_client_c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'],
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
# Text and file helpers the compositor and the agent usage collector share.
text_lib = static_library(
	'canvas-text',
	'src/core/text.cpp',
	include_directories: src_inc,
)
fs_lib = static_library(
	'canvas-fs',
	'src/core/fs.cpp',
	include_directories: src_inc,
	dependencies: [glib, json_glib],
)
desktop_entry_lib = static_library(
	'canvas-desktop-entry',
	'src/apps/desktop_entry.cpp',
	include_directories: src_inc,
	link_with: text_lib,
)

theme_prefs_lib = static_library(
	'canvas-theme-prefs',
	'src/core/theme_prefs.cpp',
	include_directories: src_inc,
	dependencies: [glib, gio],
)

# The capture toolbar's layout: a pure function of the output size, which the
# toolbar and the scenarios that click it both call.
capture_toolbar_layout_lib = static_library(
	'canvas-capture-toolbar-layout',
	'src/apps/capture_toolbar_layout.cpp',
	include_directories: src_inc,
	dependencies: core_deps,
)

session_bus_lib = static_library(
	'canvas-session-bus',
	'src/platform/session_bus.cpp',
	'src/platform/session_services.cpp',
	include_directories: src_inc,
)

# canvas.conf: the action registry, chords, parsing, resolution, the
# first-run template and the in-place edits settings.set makes.
settings_lib = static_library(
	'canvas-settings',
	'src/core/chord.cpp',
	'src/core/settings.cpp',
	'src/core/settings_file.cpp',
	include_directories: src_inc,
	dependencies: [xkbcommon],
)

# The night light's gamma ramp for a colour temperature: pure arithmetic,
# unit-tested on its own.
night_light_ramp_lib = static_library(
	'canvas-night-light-ramp',
	'src/platform/night_light_ramp.cpp',
	include_directories: src_inc,
	dependencies: [xkbcommon],
)

# The AI usage collector the agents panel's widget runs: pure logic over
# the tools' own records, unit-tested on its own, and a thin helper
# executable installed beside the compositor for the widget to run by
# name.
agent_usage_lib = static_library(
	'canvas-agent-usage',
	'src/shell/agent_usage.cpp',
	include_directories: src_inc,
	link_with: fs_lib,
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
		'src/apps/launcher_layout.cpp',
		'src/apps/capture_files.cpp',
		'src/apps/capture_geometry.cpp',
		'src/apps/capture_picker.cpp',
		'src/apps/capture_toolbar.cpp',
		'src/apps/capture_ui.cpp',
		'src/apps/clipboard_image.cpp',
		'src/apps/recorder.cpp',
		'src/apps/shot_taker.cpp',
		'src/apps/shot_thumbnail.cpp',
		'src/canvas/camera.cpp',
		'src/canvas/canvas.cpp',
		'src/canvas/desktop.cpp',
		'src/canvas/fullscreen_desktops.cpp',
		'src/canvas/overview.cpp',
		'src/canvas/overview_chrome.cpp',
		'src/core/conf_store.cpp',
		'src/core/process.cpp',
		'src/core/server.cpp',
		'src/input/bindings.cpp',
		'src/input/drag_and_drop.cpp',
		'src/input/gesture.cpp',
		'src/input/grab.cpp',
		'src/input/input.cpp',
		'src/input/pointer_router.cpp',
		'src/input/shortcut_capture.cpp',
		'src/input/super_tap.cpp',
		'src/input/tablet.cpp',
		'src/input/touch.cpp',
		'src/input/x_input_gate.cpp',
		'src/platform/canvas_surfaces.cpp',
		'src/platform/ipc_client.cpp',
		'src/platform/ipc_events.cpp',
		'src/platform/ipc_methods.cpp',
		'src/platform/ipc_methods_actions.cpp',
		'src/platform/ipc_methods_capture.cpp',
		'src/platform/ipc_methods_settings.cpp',
		'src/platform/ipc_methods_state.cpp',
		'src/platform/ipc_methods_surfaces.cpp',
		'src/platform/ipc_methods_windows.cpp',
		'src/platform/ipc_params.cpp',
		'src/platform/ipc_server.cpp',
		'src/platform/ipc_state.cpp',
		'src/platform/image_capture.cpp',
		'src/platform/lock.cpp',
		'src/platform/logind.cpp',
		'src/platform/layer.cpp',
		'src/platform/layer_shell.cpp',
		'src/platform/night_light.cpp',
		'src/platform/output.cpp',
		'src/platform/outputs.cpp',
		'src/platform/portal_chooser.cpp',
		'src/platform/session_environment.cpp',
		'src/render/capture.cpp',
		'src/render/draw.cpp',
		'src/render/hint.cpp',
		'src/render/pass.cpp',
		'src/render/render.cpp',
		'src/render/text_cache.cpp',
		'src/windows/decoration.cpp',
		'src/windows/focus.cpp',
		'src/windows/foreign_toplevel.cpp',
		'src/windows/launches.cpp',
		'src/windows/placement.cpp',
		'src/windows/popup.cpp',
		'src/windows/view.cpp',
		'src/windows/view_x11.cpp',
		'src/windows/view_xdg.cpp',
		'src/windows/xdg_shell.cpp',
		'src/windows/xwm.cpp',
	),
	protocol_headers,
	include_directories: src_inc,
	link_with: [geometry_lib, desktop_entry_lib, theme_prefs_lib, session_bus_lib,
		settings_lib, capture_toolbar_layout_lib, night_light_ramp_lib, fs_lib,
		text_lib],
	dependencies: core_deps,
)

wayland_canvas = executable(
	'wayland-canvas',
	'src/main.cpp',
	protocol_headers,
	include_directories: src_inc,
	link_with: core_lib,
	dependencies: core_deps,
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
# the display managers list, the default-session tool, the portals config
# that sends screen sharing to the wlroots backend, and that backend's
# config naming the compositor's picker as its chooser.
install_data(
	'packaging/wayland-canvas.desktop',
	install_dir: get_option('datadir') / 'wayland-sessions',
)
install_data(
	'packaging/wayland-canvas-portals.conf',
	install_dir: get_option('datadir') / 'xdg-desktop-portal',
)
install_data(
	'packaging/xdg-desktop-portal-wlr.conf',
	rename: 'wayland-canvas',
	install_dir: get_option('sysconfdir') / 'xdg' / 'xdg-desktop-portal-wlr',
)
# The lock screen's PAM service: the locker checks the password through it.
# The session's own systemd target: a login starts it, so
# graphical-session.target and XDG autostart come up with the compositor.
systemd = dependency('systemd', required: false)
systemd_user_dir = get_option('prefix') / 'lib/systemd/user'
if systemd.found()
	# under this build's prefix, not systemd's own: the same /usr/lib/systemd/user
	# for a /usr package, and inside the store path for a Nix build
	systemd_user_dir = systemd.get_variable(pkgconfig: 'systemduserunitdir',
		pkgconfig_define: ['prefix', get_option('prefix')])
endif
install_data(
	'packaging/systemd/wayland-canvas-session.target',
	install_dir: systemd_user_dir,
)
install_data(
	'packaging/pam.d/wayland-canvas',
	install_dir: get_option('sysconfdir') / 'pam.d',
)
install_data(
	'packaging/wayland-canvas-session',
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)

# ninja -C build package-arch / package-deb / packages — artifacts under
# dist/ at the repo root. tools/package.sh is the same thing without meson.
run_target(
	'package-arch',
	command: [find_program('tools/package-arch.sh'), meson.project_source_root() / 'dist'],
)
run_target(
	'package-deb',
	command: [find_program('tools/package-deb.sh'), meson.project_source_root() / 'dist'],
)
run_target(
	'packages',
	command: [find_program('tools/package.sh'), meson.project_source_root() / 'dist'],
)

# Unit tests: tests/test_<name>.cpp against the library it covers. The
# bindings test links the compositor's core: the runners are the code it
# joins to the registry, so it builds with what the scenarios build with.
test_env_vars = {'CANVAS_TMP': meson.project_source_root() / 'tmp'}
test_env = environment(test_env_vars)
unit_tests = {
	'geometry': {'link_with': geometry_lib},
	'capture-toolbar': {
		'link_with': capture_toolbar_layout_lib,
		'dependencies': core_deps,
	},
	'desktop-entry': {'link_with': desktop_entry_lib},
	'fs-text': {
		'link_with': [fs_lib, text_lib],
		'dependencies': [glib, json_glib],
	},
	'theme-prefs': {
		'link_with': theme_prefs_lib,
		'dependencies': [glib, gio],
	},
	'agent-usage': {
		'link_with': agent_usage_lib,
		'dependencies': [glib, json_glib, sqlite3],
	},
	'settings': {
		'link_with': settings_lib,
		'dependencies': [xkbcommon],
	},
	'bindings': {
		'sources': protocol_headers,
		'link_with': core_lib,
		'dependencies': core_deps,
	},
	'night-light-ramp': {'link_with': night_light_ramp_lib},
	'session-services': {'link_with': session_bus_lib},
	'session-bus': {'link_with': session_bus_lib},
}
foreach name, unit : unit_tests
	test(
		name,
		executable(
			'test-' + name,
			'tests/test_' + name.underscorify() + '.cpp',
			unit.get('sources', []),
			include_directories: src_inc,
			link_with: unit['link_with'],
			dependencies: unit.get('dependencies', []),
		),
		env: test_env,
		suite: 'unit',
	)
endforeach

# What every scenario and the media tour share (tests/harness/): the step
# driver, checks, scratch space, shots, fake devices, the control-socket
# client, child processes, the private buses and layout helpers.
harness_inc = include_directories('tests')
harness_lib = static_library(
	'canvas-harness',
	files(
		'tests/harness/bus.cpp',
		'tests/harness/check.cpp',
		'tests/harness/child.cpp',
		'tests/harness/control.cpp',
		'tests/harness/devices.cpp',
		'tests/harness/files.cpp',
		'tests/harness/image.cpp',
		'tests/harness/layout.cpp',
		'tests/harness/run.cpp',
		'tests/harness/steps.cpp',
	),
	protocol_headers,
	include_directories: [src_inc, harness_inc],
	link_with: core_lib,
	dependencies: core_deps,
)

# A test that cannot run here goes in `skipped`, with what it lacks; the end
# of this file registers it, so no run counts it as passed.
skipped = {}

# The notes' file format on a real TextEdit, under Qt 6's qmltestrunner (the
# plain `qmltestrunner` is often Qt 5's).
qml_test_runner = find_program(
	'qmltestrunner6',
	'qmltestrunner-qt6',
	'/usr/lib/qt6/bin/qmltestrunner',
	'/usr/lib64/qt6/bin/qmltestrunner',
	required: false,
)
if qml_test_runner.found()
	qml_test_env = environment()
	qml_test_env.set('QT_QPA_PLATFORM', 'offscreen')
	test(
		'note-file',
		qml_test_runner,
		args: ['-input', meson.project_source_root() / 'tests/test_note_file.qml'],
		env: qml_test_env,
		suite: 'unit',
	)
else
	skipped += {'note-file': {'suite': 'unit', 'lack': 'qmltestrunner for Qt 6 is not installed'}}
endif

# End-to-end scenarios: the real compositor on the headless backend, real foot clients, fake
# input devices, grim screenshots. They need a GPU render node at run time (headless GLES2)
# and foot and grim on PATH. The bar and session scenarios drive the shell against stand-in
# services, so they also need qs, a dbus-daemon, PipeWire's daemon and tools and the dbusmock
# environment the ensure script provisions under tmp/ at configure time; session also needs
# notify-send.
scenarios = ['gestures', 'input', 'launcher', 'ipc', 'capture', 'lock',
	'nightlight', 'touch', 'viewgone', 'xwayland',
	# adds the capture core, the capture protocols and the portal; skips its
	# portal phase itself when the tooling is missing
	'sharing', 'foreign', 'bar', 'session']
shell_tools = ['qs', 'dbus-daemon', 'pipewire', 'pw-dump', 'pw-cli', 'pw-metadata']
shell_scenarios = {'bar': [], 'session': ['notify-send']}
tools_found = {}
standins_checked = false
standins_py = ''
standins_error = ''
runnable = []
foreach scenario : scenarios
	needs = ['foot', 'grim']
	if shell_scenarios.has_key(scenario)
		needs += shell_tools + shell_scenarios[scenario]
	endif
	missing = []
	foreach tool : needs
		if not tools_found.has_key(tool)
			tools_found += {tool: find_program(tool, required: false).found()}
		endif
		if not tools_found[tool]
			missing += tool
		endif
	endforeach
	lacks = []
	if missing.length() > 0
		lacks += 'not on PATH: ' + ', '.join(missing)
	endif
	if lacks.length() == 0 and shell_scenarios.has_key(scenario)
		if not standins_checked
			standins_checked = true
			standins = run_command('tests/scenarios/ensure-bar-standins.sh', check: false)
			if standins.returncode() == 0
				standins_py = standins.stdout().strip().split('\n')[-1]
			else
				standins_error = standins.stderr().strip().split('\n')[-1]
				if standins_error == ''
					standins_error = 'tests/scenarios/ensure-bar-standins.sh exited ' + standins.returncode().to_string()
				endif
			endif
		endif
		if standins_py == ''
			lacks += standins_error
		endif
	endif
	if lacks.length() > 0
		skipped += {scenario: {'suite': 'scenarios', 'lack': '; '.join(lacks)}}
	else
		runnable += scenario
	endif
endforeach
if runnable.length() > 0
	scenario_env = environment(test_env_vars)
	scenario_env.set('WAYLAND_CANVAS_BIN', wayland_canvas.full_path())
	# The source tree, for scenarios that install a shipped file as-is.
	scenario_env.set('CANVAS_SOURCE', meson.project_source_root())
	# The capture scenario's stand-in encoder: an ffmpeg that never reads
	# its input, put first on PATH for the recording it stalls.
	scenario_env.set(
		'CANVAS_RECORDER_STANDIN',
		meson.project_source_root() / 'tests/scenarios/recorder-standins/ffmpeg',
	)
	# A locker of the scenarios' own: no swaylock or hyprlock is needed to
	# prove the protocol.
	lock_client = executable(
		'lock-client',
		'tests/clients/lock-client.c',
		client_protocols,
		c_args: ['-Wno-unused-parameter'],
		dependencies: [wayland_client],
	)
	scenario_env.set('CANVAS_LOCK_CLIENT', lock_client.full_path())
	# A gamma-control client of the scenarios' own: no wlsunset or
	# gammastep is needed to prove the protocol.
	gamma_client = executable(
		'gamma-client',
		'tests/clients/gamma-client.c',
		client_protocols,
		c_args: ['-Wno-unused-parameter'],
		dependencies: [wayland_client],
	)
	scenario_env.set('CANVAS_GAMMA_CLIENT', gamma_client.full_path())
	# A layer-shell client of the scenarios' own: a plugin's surface on the
	# canvas, mapped and pressed without the shell.
	layer_client = executable(
		'layer-client',
		'tests/clients/layer-client.c',
		client_protocols,
		c_args: ['-Wno-unused-parameter'],
		dependencies: [wayland_client],
	)
	scenario_env.set('CANVAS_LAYER_CLIENT', layer_client.full_path())
	# A window-cast client of the scenarios' own: it holds an
	# ext-image-copy-capture session of one window and copies its frames
	# until the compositor ends the session.
	cast_client = executable(
		'cast-client',
		'tests/clients/cast-client.c',
		client_protocols,
		c_args: ['-Wno-unused-parameter'],
		dependencies: [wayland_client],
	)
	scenario_env.set('CANVAS_CAST_CLIENT', cast_client.full_path())
	# A taskbar of the scenarios' own: it lists the windows through
	# wlr-foreign-toplevel-management and asks for changes to them, and what
	# the compositor publishes is asserted from its own stdout.
	foreign_client = executable(
		'foreign-client',
		'tests/clients/foreign-client.c',
		client_protocols,
		c_args: ['-Wno-unused-parameter'],
		dependencies: [wayland_client],
	)
	scenario_env.set('CANVAS_FOREIGN_CLIENT', foreign_client.full_path())
	# A touch, pointer and tablet-tool client of the scenarios' own:
	# what the compositor forwards of a touchscreen or pen is asserted
	# from its own stdout.
	touch_client = executable(
		'touch-client',
		'tests/clients/touch-client.c',
		client_protocols,
		c_args: ['-Wno-unused-parameter'],
		dependencies: [wayland_client],
	)
	scenario_env.set('CANVAS_TOUCH_CLIENT', touch_client.full_path())
	# A pointer-gestures client of the scenarios' own: a small layer-shell
	# square in the output's corner reports every swipe and pinch the
	# compositor forwards to it, so "forwarded to the client" is read from
	# what a client received.
	gesture_client = executable(
		'gesture-client',
		'tests/clients/gesture-client.c',
		client_protocols,
		c_args: ['-Wno-unused-parameter'],
		dependencies: [wayland_client],
	)
	scenario_env.set('CANVAS_GESTURE_CLIENT', gesture_client.full_path())
	# An X11 client of the scenarios' own: this machine has no xterm or
	# xeyes, and the X11 half must be proven by a real X client.
	x11_client = executable(
		'x11-client',
		'tests/clients/x11-client.c',
		c_args: ['-Wno-unused-parameter'],
		dependencies: [xcb],
	)
	scenario_env.set('CANVAS_X11_CLIENT', x11_client.full_path())
	if standins_py != ''
		scenario_env.set('CANVAS_BAR_STANDIN_PY', standins_py)
		scenario_env.set(
			'CANVAS_BAR_STANDINS',
			meson.project_source_root() / 'tests/scenarios/bar-standins.py',
		)
		# The agents panel's widget runs the collector by name; the bar
		# scenario points its PATH at the build tree's copy.
		scenario_env.set('WAYLAND_CANVAS_AGENT_USAGE', agent_usage.full_path())
	endif
	# The bar scenario is a file per phase under tests/scenarios/bar/.
	bar_parts = [
		'agent_helpers',
		'agents',
		'appbar',
		'appbar_hover',
		'barwrites',
		'bring_up',
		'bringup',
		'edge_helpers',
		'edges',
		'handover',
		'main',
		'network',
		'nightlight',
		'note_helpers',
		'notes',
		'overview',
		'pixels',
		'plugin_settings',
		'plugins',
		'recording',
		'screenshots',
		'settings',
		'settings_helpers',
		'shared',
		'socket',
		'standins',
		'starts',
		'surfaces',
		'theme',
		'toolbar',
		'walk',
		'watcher',
		'wheel',
		'world',
	]
	foreach scenario : runnable
		scenario_sources = ['tests/scenarios/' + scenario + '.cpp']
		if scenario == 'bar'
			scenario_sources = []
			foreach part : bar_parts
				scenario_sources += 'tests/scenarios/bar/' + part + '.cpp'
			endforeach
		endif
		# The gestures scenario counts the allocations of its drag and pinch
		# frames. A sanitizer owns malloc, so its builds leave the counter
		# out and the scenario says so.
		if scenario == 'gestures' and get_option('b_sanitize') == 'none'
			scenario_sources += 'tests/scenarios/alloc-counter.c'
		endif
		exe = executable(
			'scenario-' + scenario,
			scenario_sources,
			protocol_headers,
			include_directories: [src_inc, harness_inc],
			link_with: [harness_lib, core_lib],
			dependencies: core_deps,
		)
		# Scenarios run side by side: each has its own display, control
		# socket, scratch folder and buses.
		if scenario != 'bar'
			test(scenario, exe, suite: 'scenarios', timeout: 180, env: scenario_env)
			continue
		endif
		# Each bar phase is a test of its own after the bring-up every run
		# takes, so a change runs only the phases that cover it
		# (`meson test -C build --suite bar` runs them all); bar-phases fails
		# when the runner has a phase this list lacks.
		bar_phases = ['walk', 'plugins', 'overview', 'toolbar', 'theme', 'watcher',
			'nightlight', 'wheel', 'notes', 'network', 'agents', 'recording', 'edges',
			'barwrites', 'socket', 'settings', 'appbar', 'plugin_settings',
			'appbar_hover', 'screenshots', 'shared', 'handover']
		test('bar-phases', exe, args: ['--phases', ','.join(bar_phases)],
			suite: 'scenarios', env: scenario_env)
		foreach phase : bar_phases
			test('bar-' + phase, exe, args: [phase], suite: ['scenarios', 'bar'],
				timeout: 300, env: scenario_env)
		endforeach
	endforeach
endif

# The README's media: the tour stages the canvas at work with real apps on
# the headless backend, takes its stills and records its walkthrough;
# tools/media.py runs it. Built with everything, so it keeps compiling
# against the compositor it shows.
executable(
	'media-tour',
	'tools/media/tour.cpp',
	protocol_headers,
	include_directories: [src_inc, harness_inc],
	link_with: [harness_lib, core_lib],
	dependencies: core_deps,
)

# The default-session tool against fake roots, one per display manager.
test(
	'packaging',
	find_program('tests/test_packaging.sh'),
	suite: 'packaging',
	is_parallel: false,
	timeout: 120,
	env: test_env,
)

# The plan tools, the autopilot hook and agent-setup.sh against scratch copies.
test(
	'agent-tools',
	find_program('tests/test_agent_tools.sh'),
	suite: 'packaging',
	is_parallel: false,
	timeout: 120,
	env: test_env,
)

# A test that cannot run here is registered all the same, as one that exits
# 77 (meson's SKIP) and names what it lacks.
skip_test = find_program('tests/skip.sh')
foreach name, skip : skipped
	test(name, skip_test, args: [name, skip['lack']], suite: skip['suite'])
	summary(name, skip['lack'], section: 'Skipped tests')
endforeach
