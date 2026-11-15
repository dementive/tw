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
libudev = dependency('libudev')   # the power cable; wlroots uses it already
libm = cc.find_library('m')

# --- The machine ---------------------------------------------------------------

# What depends on the machine is in src/machines/<name>.h, which config.h
# includes. 'auto' takes the file named after this machine's board if there
# is one, and default.h otherwise.
fs = import('fs')
machine = get_option('machine')
if machine == 'auto'
	machine = 'default'
	# Read inside a shell command. Named to Meson as a file, by fs.read() or
	# as an argument of its own, it would become something the build depends
	# on: it looks new after every boot, and the first build after each one
	# would set itself up again.
	board = run_command('sh', '-c', 'cat /sys/class/dmi/id/board_name 2>/dev/null', check: false)
	board = board.stdout().strip().to_lower().underscorify()
	if board != '' and fs.is_file('src/machines' / board + '.h')
		machine = board
	endif
endif
if not fs.is_file('src/machines' / machine + '.h')
	error('there is no src/machines/@0@.h; copy src/machines/default.h to make one'.format(machine))
endif
add_project_arguments('-DTW_MACHINE_H="machines/@0@.h"'.format(machine), language: 'c')
summary('machine', 'src/machines/@0@.h'.format(machine), section: 'tw')

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
	'xdg-activation-v1': wl_protocol_dir / 'staging/xdg-activation/xdg-activation-v1.xml',
	'viewporter': wl_protocol_dir / 'stable/viewporter/viewporter.xml',
	'fractional-scale-v1': wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
	'pointer-constraints-unstable-v1': wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
	'relative-pointer-unstable-v1': wl_protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
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
	'src/input.c',
	'src/window.c',
	'src/layer.c',
	'src/clipboard.c',
	'src/idle.c',
	'src/lock.c',
	'src/x11.c',
	'src/power.c',
	'src/layout.c',
	# wlroots' layer-shell header includes this generated one.
	server_header['wlr-layer-shell-unstable-v1'],
	dependencies: [wlroots, wayland_server, xkbcommon, libinput, libudev, libm],
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

# Lets the NVIDIA GPU power down once its driver is gone.
udev_rules = {'gpu': 'data/tw-gpu-udev.rules'}
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

# After the files are in place, has systemd, udev and polkit read them.
meson.add_install_script('data/post-install.sh')

# --- The notification service ------------------------------------------------

# tw-notify shows notifications and is gone again when none is left. It runs
# on libc and the Wayland client library alone: the characters it can show
# are drawn here, at build time, by a tool that uses FreeType (notify/bake.c).
freetype = dependency('freetype2', native: true, required: get_option('notify'))
fc_match = find_program('fc-match', native: true, required: get_option('notify'))
tw_notify = disabler()
if freetype.found() and fc_match.found()
	notify_font = run_command(
		fc_match, '--format=%{file}', get_option('notify_font'),
		check: true,
	).stdout().strip()
	notify_bake = executable(
		'tw-notify-bake',
		'notify/bake.c',
		dependencies: freetype,
		native: true,
	)
	notify_font_header = custom_target(
		'font.h',
		input: notify_font,
		output: 'font.h',
		command: [notify_bake, '@INPUT@'],
		capture: true,
	)
	tw_notify = executable(
		'tw-notify',
		'notify/notify.c',
		'notify/bus.c',
		notify_font_header,
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
		dependencies: wayland_client,
		install: true,
	)
	# How the session's D-Bus knows what to start for a notification. In
	# /usr/local it is read before the files of other notification services
	# in /usr/share, so it wins over them without their being removed.
	configure_file(
		input: 'data/org.freedesktop.Notifications.service.in',
		output: 'org.freedesktop.Notifications.service',
		configuration: {'bindir': get_option('prefix') / get_option('bindir')},
		install: true,
		install_dir: get_option('datadir') / 'dbus-1/services',
	)
endif

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

# The scripts, each against a made-up machine: no hardware, service or root
# is involved, and nothing of the real machine is read or written. The value
# is the directory the scripts under test are in.
script_tests = {
	'gpu': 'data',         # tw-gpu, its root helper, and tw-session
	'profile': 'data',     # tw-power and its root helper
	'bluetooth': 'data',   # tw-bluetooth and its root helper
	'thunderbolt': 'data', # the Thunderbolt udev rule
	'install': 'data',     # the step that runs after `meson install`
	'iwd': 'tools',        # tools/iwd.sh
	'keyring': 'tools',    # tools/keyring.sh
}
foreach name, dir : script_tests
	test(
		name,
		find_program('tests' / name + '.sh'),
		args: [meson.current_source_dir() / dir],
		timeout: 120,
	)
endforeach

# Budgets only mean something for an optimized build. Ratchet these down as
# the numbers improve; raising one needs a reason in the commit message.
#
# The PSS figure divides each library page among the processes that map it,
# so it moves with what else runs on the machine: one and the same commit
# read 2320 to 2484 kB with a login manager's compositor running and 2451 to
# 2536 kB without one. What tw allocated does not move, so that is the
# figure that catches a regression.
budget_idle_wakeups = 0   # context switches in 3 s with idle clients mapped
budget_own_kb = 2800      # PSS minus pixel buffers, 3 clients mapped (measured: 2445 to 2713)
budget_alloc_kb = 850     # of which allocated by tw itself (measured: 808 to 812)
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
