project(
	'tw', 'c',
	version: '0.1.0',
	meson_version: '>=1.3',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'b_ndebug=if-release',
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

# wlroots breaks its API on every 0.x release, so tw targets exactly one:
# the pinned source in subprojects/wlroots, linked statically. With
# -Db_lto=true everything tw does not use (unused backends, renderers and
# protocols) is dropped from the binary, and no system update can change
# wlroots under an installed tw.
#
# The libraries below come from the system (docs/setup.md). Where one is
# missing or older than wlroots asks for, wlroots would fetch it from the
# latest commit through its own wrap files, unpinned, and build tw against
# that. Asking for each here first, at wlroots' own minimum and with no
# fallback, hands wlroots the system's copy or stops the build and names the
# package. The versions are those in subprojects/wlroots/meson.build and move
# with the wlroots pin. allow_fallback: false matters even without a fallback
# named: Meson falls back on its own to any wrap file named after the
# dependency, and it leaves redirects such as wayland-protocols.wrap in
# subprojects/.
dependency('libseat', version: '>=0.2.0', allow_fallback: false)
wayland_protos = dependency('wayland-protocols', version: '>=1.47',
	allow_fallback: false)
wayland_server = dependency('wayland-server', version: '>=1.24.0',
	allow_fallback: false)
wayland_scanner_dep = dependency('wayland-scanner', version: '>=1.24.0',
	native: true, allow_fallback: false)
dependency('libdrm', version: '>=2.4.129', allow_fallback: false)
xkbcommon = dependency('xkbcommon', version: '>=1.8.0', allow_fallback: false)
dependency('pixman-1', version: '>=0.43.0', allow_fallback: false)

# libdisplay-info, with which wlroots reads what a monitor says about itself,
# is the one library under tw whose file name changes with its releases
# (libdisplay-info.so.3). A system update that brings the next one would
# leave an installed tw unable to start until it is built again. So it is
# built in as well, from a source pinned like wlroots'. Set up first, it is
# the one wlroots finds.
subproject(
	'libdisplay-info',
	default_options: ['default_library=static', 'werror=false'],
)
wlroots = subproject(
	'wlroots',
	version: ['>=0.20.0', '<0.21.0'],
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

wayland_client = dependency('wayland-client')
libinput = dependency('libinput') # touchpad settings
libudev = dependency('libudev')   # the power cable; wlroots uses it already
# To free Mesa's shader compiler once wlroots' shaders are built; wlroots
# uses both already.
egl = dependency('egl')
glesv2 = dependency('glesv2')
libm = cc.find_library('m')

# --- Protocols ---------------------------------------------------------------

wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

# The wlr and virtual-keyboard protocols are not part of wayland-protocols, so
# their XML is kept in protocols/ (copied from the pinned wlroots).
protocols = {
	'xdg-shell': wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	'ext-session-lock-v1': wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
	'idle-inhibit-unstable-v1': wl_protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
	'xdg-activation-v1': wl_protocol_dir / 'staging/xdg-activation/xdg-activation-v1.xml',
	'viewporter': wl_protocol_dir / 'stable/viewporter/viewporter.xml',
	'fractional-scale-v1': wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
	'pointer-constraints-unstable-v1': wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
	'relative-pointer-unstable-v1': wl_protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
	'cursor-shape-v1': wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
	'tablet-v2': wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
	'wlr-layer-shell-unstable-v1': 'protocols/wlr-layer-shell-unstable-v1.xml',
	'virtual-keyboard-unstable-v1': 'protocols/virtual-keyboard-unstable-v1.xml',
	'wlr-data-control-unstable-v1': 'protocols/wlr-data-control-unstable-v1.xml',
	'wlr-virtual-pointer-unstable-v1': 'protocols/wlr-virtual-pointer-unstable-v1.xml',
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
	'src/layer.c',
	'src/window.c',
	'src/strip.c',
	'src/input.c',
	'src/clipboard.c',
	'src/idle.c',
	'src/lock.c',
	'src/power.c',
	'src/x11.c',
	'src/layout.c',
	# wlroots' layer-shell header includes this generated one.
	server_header['wlr-layer-shell-unstable-v1'],
	dependencies: [wlroots, wayland_server, xkbcommon, libinput, libudev, egl, glesv2, libm],
	install: true,
)

# --- What is installed with it ---------------------------------------------------

bindir = get_option('bindir')
libexecdir = get_option('libexecdir')
datadir = get_option('datadir')
# systemd, udev and modprobe read these under the prefix too (/usr/local/lib).
systemd_system_dir = get_option('prefix') / 'lib/systemd/system'
systemd_user_dir = get_option('prefix') / 'lib/systemd/user'
udev_rules_dir = get_option('prefix') / 'lib/udev/rules.d'
modprobe_dir = get_option('prefix') / 'lib/modprobe.d'
paths = {
	'bindir': get_option('prefix') / bindir,
	'libexecdir': get_option('prefix') / libexecdir,
	'conf': modprobe_dir / 'tw-bluetooth.conf',
}

# The session: what the login screen lists, and what a console login types.
install_data('data/tw-session', install_dir: bindir, install_mode: 'rwxr-xr-x')
configure_file(
	input: 'data/tw.desktop.in',
	output: 'tw.desktop',
	configuration: paths,
	install: true,
	install_dir: datadir / 'wayland-sessions',
)
# Tells xdg-desktop-portal which backends to use in a tw session.
install_data('data/tw-portals.conf', install_dir: datadir / 'xdg-desktop-portal')
# The line added to the accessibility bus's unit, which keeps the session
# bus from starting it in a session that tw-session told to leave it alone.
install_data(
	'data/tw-a11y.conf',
	rename: 'tw.conf',
	install_dir: systemd_user_dir / 'at-spi-dbus-bus.service.d',
)
# What logind does with the power key: nothing at a short press, which a
# desktop would otherwise have taken over.
install_data(
	'data/tw-logind.conf',
	rename: 'tw.conf',
	install_dir: get_option('prefix') / 'lib/systemd/logind.conf.d',
)

# Three commands that need root for what they do, each built the same way:
# the command itself, a root helper, the system unit that runs the helper,
# and the polkit rule that lets the user start that unit.
#   tw-gpu        keeps a laptop's NVIDIA GPU off until a program wants it
#   tw-power      sets the power profile when the power cable goes in or out
#   tw-bluetooth  keeps Bluetooth off until asked for
# Each does nothing on a machine without the hardware it is about.
root_commands = ['gpu', 'power']
if get_option('bluetooth')
	root_commands += 'bluetooth'
endif
foreach name : root_commands
	install_data('data/tw-' + name, install_dir: bindir, install_mode: 'rwxr-xr-x')
	install_data('data/tw-' + name + '-root', install_dir: libexecdir, install_mode: 'rwxr-xr-x')
	configure_file(
		input: 'data/tw-' + name + '@.service.in',
		output: 'tw-' + name + '@.service',
		configuration: paths,
		install: true,
		install_dir: systemd_system_dir,
	)
	install_data(
		'data/tw-' + name + '.rules',
		rename: '50-tw-' + name + '.rules',
		install_dir: datadir / 'polkit-1/rules.d',
	)
endforeach

# Lets the NVIDIA GPU power down once its driver is gone, and a laptop's
# built-in keyboard sleep between keys.
udev_rules = {'gpu': 'data/tw-gpu-udev.rules', 'keyboard': 'data/tw-keyboard.rules'}
if get_option('bluetooth')
	# What keeps the adapter's driver from loading as the machine starts, the
	# timer that switches Bluetooth off again when nothing is connected, the
	# line that keeps the system bus from starting the Bluetooth service
	# while Bluetooth is off, and the udev rules that start that timer and
	# let a driverless adapter sleep.
	install_data('data/tw-bluetooth.conf', install_dir: modprobe_dir)
	install_data('data/tw-bluetooth-idle.timer', install_dir: systemd_system_dir)
	install_data(
		'data/tw-bluetooth-service.conf',
		rename: 'tw-bluetooth.conf',
		install_dir: systemd_system_dir / 'bluetooth.service.d',
	)
	udev_rules += {'bluetooth': 'data/tw-bluetooth-udev.rules'}
endif
if get_option('thunderbolt')
	# Lets a Thunderbolt device in as it is plugged in, where the kernel
	# protects its memory from such devices: a dock without boltd.
	udev_rules += {'thunderbolt': 'data/tw-thunderbolt.rules'}
endif
foreach name, file : udev_rules
	install_data(file, rename: '72-tw-' + name + '.rules', install_dir: udev_rules_dir)
endforeach


# --- The notification service ------------------------------------------------

# tw-notify shows notifications and is gone again when none is left. It
# speaks D-Bus by itself (notify/bus.c) and draws its text with fontconfig
# and FreeType, which it loads only while a notification is on screen.
fontconfig = dependency('fontconfig', required: get_option('notify'))
freetype = dependency('freetype2', required: get_option('notify'))
tw_notify = disabler()
if fontconfig.found() and freetype.found()
	tw_notify = executable(
		'tw-notify',
		'notify/notify.c',
		'notify/bus.c',
		client_header['wlr-layer-shell-unstable-v1'],
		protocol_code['wlr-layer-shell-unstable-v1'],
		# The layer shell's code refers to xdg-shell's popups.
		protocol_code['xdg-shell'],
		client_header['viewporter'],
		protocol_code['viewporter'],
		client_header['fractional-scale-v1'],
		protocol_code['fractional-scale-v1'],
		client_header['xdg-activation-v1'],
		protocol_code['xdg-activation-v1'],
		client_header['cursor-shape-v1'],
		protocol_code['cursor-shape-v1'],
		# Cursor-shape's code refers to the tablet protocol's tools.
		protocol_code['tablet-v2'],
		dependencies: [wayland_client, fontconfig, freetype],
		install: true,
	)
	# How the session's D-Bus knows what to start for a notification. In
	# /usr/local it is read before the files of other notification services
	# in /usr/share, so it wins over them without their being removed.
	configure_file(
		input: 'data/org.freedesktop.Notifications.service.in',
		output: 'org.freedesktop.Notifications.service',
		configuration: paths,
		install: true,
		install_dir: datadir / 'dbus-1/services',
	)
endif

# --- After the install ---------------------------------------------------------

# meson install never removes a file, so one an earlier install put there
# for an option that is now off would stay and keep working: Bluetooth
# kept off, another notification service shadowed. These are removed, and
# then systemd, udev and polkit read what is there.
prefix = get_option('prefix')
left_over = []
if not get_option('bluetooth')
	left_over += [
		prefix / bindir / 'tw-bluetooth',
		prefix / libexecdir / 'tw-bluetooth-root',
		systemd_system_dir / 'tw-bluetooth@.service',
		systemd_system_dir / 'tw-bluetooth-idle.timer',
		systemd_system_dir / 'bluetooth.service.d/tw-bluetooth.conf',
		prefix / datadir / 'polkit-1/rules.d/50-tw-bluetooth.rules',
		modprobe_dir / 'tw-bluetooth.conf',
		udev_rules_dir / '72-tw-bluetooth.rules',
	]
endif
if not get_option('thunderbolt')
	left_over += udev_rules_dir / '72-tw-thunderbolt.rules'
endif
if not (fontconfig.found() and freetype.found())
	left_over += [
		prefix / bindir / 'tw-notify',
		prefix / datadir / 'dbus-1/services/org.freedesktop.Notifications.service',
	]
endif
meson.add_install_script('data/post-install.sh', left_over)

# --- Tests -------------------------------------------------------------------

# Pure layout maths: no compositor needed.
test('layout', executable(
	'layout-test',
	'tests/layout_test.c',
	'src/layout.c',
	include_directories: 'src',
	dependencies: libm,
))

# The D-Bus messages of tw-notify: reading ones that are cut short or wrong.
test('bus', executable(
	'bus-test',
	'tests/bus_test.c',
	'notify/bus.c',
	include_directories: 'notify',
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
	client_header['xdg-activation-v1'],
	protocol_code['xdg-activation-v1'],
	client_header['pointer-constraints-unstable-v1'],
	protocol_code['pointer-constraints-unstable-v1'],
	client_header['relative-pointer-unstable-v1'],
	protocol_code['relative-pointer-unstable-v1'],
	client_header['fractional-scale-v1'],
	protocol_code['fractional-scale-v1'],
	dependencies: wayland_client,
)
# Copies and pastes the way a clipboard tool does.
test_clip = executable(
	'tw-test-clip',
	'tests/clip.c',
	client_header['wlr-data-control-unstable-v1'],
	protocol_code['wlr-data-control-unstable-v1'],
	dependencies: wayland_client,
)
test_keys = executable(
	'tw-test-keys',
	'tests/keys.c',
	client_header['virtual-keyboard-unstable-v1'],
	protocol_code['virtual-keyboard-unstable-v1'],
	client_header['wlr-virtual-pointer-unstable-v1'],
	protocol_code['wlr-virtual-pointer-unstable-v1'],
	dependencies: [wayland_client, xkbcommon],
)

# End-to-end on the headless backend, one compositor per test. In a sanitizer
# build these are also the lifetime-bug and leak check.
headless_tests = {
	# everything a client can observe: sizes, focus, bindings, workspaces, lock
	'smoke': [tw, test_client, test_keys],
	# windows sharing a column, floating dialogs
	'columns': [tw, test_client, test_keys],
	# the mouse: pointer focus, click to focus, pointer lock and confinement
	'pointer': [tw, test_client, test_keys],
	# two monitors: a workspace on each, focus and columns between them
	'outputs': [tw, test_client, test_keys],
	# screen blanking, inhibitors, locking on idle, with a two-second timeout
	'idle': [tw, test_client, test_keys],
	# X11 sockets, starting and stopping the helper, with a stand-in helper
	'x11': [tw, test_client],
	# a copy outlives the program it was made in
	'clipboard': [tw, test_client, test_clip],
	# what tw does on battery and on mains, against made-up power supplies
	'power': [tw, test_client],
	# tw-notify on a private session bus; skipped where it is not built
	'notify': [tw, test_client, test_keys, tw_notify],
}
foreach name, args : headless_tests
	test(
		name,
		find_program('tests' / name + '.sh'),
		args: args,
		is_parallel: false,
		timeout: 120,
	)
endforeach

# The scripts in data/, each against a made-up machine: no hardware, service
# or root is involved, and nothing of the real machine is read or written.
script_tests = {
	'gpu': 'tw-gpu, its root helper, and tw-session',
	'profile': 'tw-power and its root helper',
	'bluetooth': 'tw-bluetooth and its root helper',
	'thunderbolt': 'the Thunderbolt udev rule',
	'keyboard': 'the built-in keyboard udev rule',
	'install': 'the step that runs after `meson install`',
}
foreach name, what : script_tests
	test(
		name,
		find_program('tests' / name + '.sh'),
		args: [meson.current_source_dir() / 'data'],
		timeout: 120,
	)
endforeach

# Budgets only mean something for an optimized build. Ratchet these down as
# the numbers improve; raising one needs a reason in the commit message.
# The PSS figure moves by 100 to 200 kB with what else runs on the machine;
# what tw allocated does not, so that is the figure that catches a
# regression. docs/testing.md has the measured ranges.
budget_idle_wakeups = 0   # context switches in 3 s with idle clients mapped
budget_own_kb = 2800      # PSS minus pixel buffers, 3 clients mapped
budget_alloc_kb = 850     # of which allocated by tw itself
if get_option('buildtype') == 'release'
	test(
		'budget',
		find_program('tests/budget.sh'),
		args: [
			tw, test_client, budget_idle_wakeups.to_string(),
			budget_own_kb.to_string(), budget_alloc_kb.to_string(),
		],
		suite: 'budget',
		is_parallel: false,
		timeout: 60,
	)
endif
