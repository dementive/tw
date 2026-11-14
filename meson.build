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
wayland_client = dependency('wayland-client')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput') # touchpad settings
libm = cc.find_library('m')

# --- Protocols ---------------------------------------------------------------

# Declared after wlroots, which can supply wayland-protocols as a subproject.
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

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

# The wlr and virtual-keyboard protocols are not part of wayland-protocols, so
# their XML is kept in protocols/ (copied from the pinned wlroots).
protocols = {
	'xdg-shell': wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	'ext-session-lock-v1': wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
	'idle-inhibit-unstable-v1': wl_protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
	'wlr-layer-shell-unstable-v1': 'protocols/wlr-layer-shell-unstable-v1.xml',
	'virtual-keyboard-unstable-v1': 'protocols/virtual-keyboard-unstable-v1.xml',
}
server_header = {}
client_header = {}
protocol_code = {}
foreach name, xml : protocols
	server_header += {name: custom_target(
		name + '-protocol.h',
		input: xml,
		output: name + '-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)}
	client_header += {name: custom_target(
		name + '-client-protocol.h',
		input: xml,
		output: name + '-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)}
	protocol_code += {name: custom_target(
		name + '-protocol.c',
		input: xml,
		output: name + '-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)}
endforeach

# --- The compositor ----------------------------------------------------------

tw = executable(
	'tw',
	'src/main.c',
	'src/output.c',
	'src/input.c',
	'src/window.c',
	'src/layer.c',
	'src/idle.c',
	'src/lock.c',
	'src/layout.c',
	# wlroots' layer-shell header includes this generated one.
	server_header['wlr-layer-shell-unstable-v1'],
	dependencies: [wlroots, wayland_server, xkbcommon, libinput, libm],
	install: true,
)

# Login-screen entry: `sudo meson install` makes "tw" selectable next to the
# other sessions.
install_data(
	'data/tw-session',
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)
configure_file(
	input: 'data/tw.desktop.in',
	output: 'tw.desktop',
	configuration: {'bindir': get_option('prefix') / get_option('bindir')},
	install: true,
	install_dir: get_option('datadir') / 'wayland-sessions',
)

# --- Tests -------------------------------------------------------------------

# Pure layout maths: no compositor needed.
test('layout', executable(
	'layout-test',
	'tests/layout_test.c',
	'src/layout.c',
	include_directories: 'src',
	dependencies: libm,
))

# A minimal client that reports the state the compositor gives it, and a tool
# that presses keys through the virtual-keyboard protocol.
test_client = executable(
	'tw-test-client',
	'tests/client.c',
	client_header['xdg-shell'],
	protocol_code['xdg-shell'],
	client_header['wlr-layer-shell-unstable-v1'],
	protocol_code['wlr-layer-shell-unstable-v1'],
	client_header['ext-session-lock-v1'],
	protocol_code['ext-session-lock-v1'],
	client_header['idle-inhibit-unstable-v1'],
	protocol_code['idle-inhibit-unstable-v1'],
	dependencies: wayland_client,
)
test_keys = executable(
	'tw-test-keys',
	'tests/keys.c',
	client_header['virtual-keyboard-unstable-v1'],
	protocol_code['virtual-keyboard-unstable-v1'],
	dependencies: [wayland_client, xkbcommon],
)

# End-to-end on the headless backend. In a sanitizer build this is also the
# lifetime-bug and leak check.
test(
	'smoke',
	find_program('tests/smoke.sh'),
	args: [tw, test_client, test_keys],
	is_parallel: false,
	timeout: 120,
)

# Windows sharing a column, and floating dialogs.
test(
	'columns',
	find_program('tests/columns.sh'),
	args: [tw, test_client, test_keys],
	is_parallel: false,
	timeout: 120,
)

# Screen blanking, with a one-second timeout so the test does not take minutes.
test(
	'idle',
	find_program('tests/idle.sh'),
	args: [tw, test_client, test_keys],
	is_parallel: false,
	timeout: 120,
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
