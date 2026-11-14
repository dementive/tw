project(
	'tw', 'c',
	version: '0.1.0',
	meson_version: '>=1.3',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'b_ndebug=if-release',
		'seatd:default_library=static',
	],
)

cc = meson.get_compiler('c')

add_project_arguments(
	'-D_POSIX_C_SOURCE=200809L',
	'-DWLR_USE_UNSTABLE',
	cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wshadow',
		'-Wvla',
		'-Wstrict-prototypes',
		'-Wmissing-prototypes',
		'-Wundef',
	]),
	language: 'c',
)

# wlroots breaks its API on every 0.x release, so tw targets exactly one.
# The default build compiles the pinned source in subprojects/wlroots and
# links it statically: with -Db_lto=true everything tw does not use (unused
# backends, renderers and protocols) is dropped from the binary.
wlroots_version = ['>=0.20.0', '<0.21.0']
if get_option('vendored_wlroots')
	wlroots = subproject(
		'wlroots',
		version: wlroots_version,
		default_options: [
			'default_library=static',
			'werror=false',
			'examples=false',
			'xwayland=disabled',
			'xcb-errors=disabled',
			'backends=drm,libinput',
			'renderers=gles2',
			'allocators=gbm',
			'session=enabled',
			'color-management=disabled',
			'libliftoff=disabled',
		],
	).get_variable('wlroots')
else
	wlroots = dependency('wlroots-0.20', version: wlroots_version)
endif

wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

tw = executable(
	'tw',
	'src/main.c',
	'src/output.c',
	'src/input.c',
	'src/window.c',
	'src/layout.c',
	dependencies: [wlroots, wayland_server, xkbcommon],
	install: true,
)

# --- Tests -----------------------------------------------------------------

# Pure layout maths: no compositor needed.
test('layout', executable(
	'layout-test',
	'tests/layout_test.c',
	'src/layout.c',
	include_directories: 'src',
))

# A minimal xdg-shell client that reports the state the compositor gives it.
wayland_protos = dependency(
	'wayland-protocols',
	version: '>=1.32',
	fallback: 'wayland-protocols',
	default_options: ['tests=false'],
)
wayland_scanner = find_program(
	dependency('wayland-scanner', native: true).get_variable('wayland_scanner'),
	native: true,
)
xdg_shell_xml = wayland_protos.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_client_h = custom_target(
	'xdg-shell-client-protocol.h',
	input: xdg_shell_xml,
	output: 'xdg-shell-client-protocol.h',
	command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_c = custom_target(
	'xdg-shell-protocol.c',
	input: xdg_shell_xml,
	output: 'xdg-shell-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
test_client = executable(
	'tw-test-client',
	'tests/client.c',
	xdg_shell_client_h,
	xdg_shell_c,
	dependencies: dependency('wayland-client'),
)

# End-to-end on the headless backend. In a sanitizer build this is also the
# lifetime-bug and leak check.
test(
	'smoke',
	find_program('tests/smoke.sh'),
	args: [tw, test_client],
	is_parallel: false,
	timeout: 60,
)

# Budgets only mean something for an optimized build. Ratchet these down as
# the numbers improve; raising one needs a reason in the commit message.
budget_idle_wakeups = 0   # context switches in 3 s with idle clients mapped
budget_own_kb = 2500      # PSS minus pixel buffers, 3 clients mapped (measured: ~1950)
if get_option('buildtype') == 'release'
	test(
		'budget',
		find_program('tests/budget.sh'),
		args: [tw, test_client, budget_idle_wakeups.to_string(), budget_own_kb.to_string()],
		suite: 'budget',
		is_parallel: false,
		timeout: 60,
	)
endif
