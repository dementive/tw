#!/bin/sh
# tw-bluetooth and its root helper, run against a made-up machine: one
# directory standing in for /sys and one for /run, and stand-ins for
# modprobe, systemctl, udevadm and sleep that load and unload pretend
# modules, start and stop a pretend Bluetooth service, start it again the
# way the system bus does, and do to the radio's block what systemd does.
# Nothing here touches a real adapter, driver or service, or needs root.
# What it cannot show: that the real modules unload and load again, that
# systemd starts the service when the adapter appears, that the polkit rule
# lets the units be started, or that the udev rules do what they say.
# $1 = the data/ directory of the source tree.
set -eu
data=$1
tmp=$(mktemp -d "$PWD/bluetooth-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
sys=$tmp/sys
run=$tmp/run         # stands in for /run: where the root helper leaves its note
bin=$tmp/bin         # stand-ins for tw-bluetooth: systemctl
rootbin=$tmp/rootbin # stand-ins for tw-bluetooth-root
state=$tmp/state     # what the made-up machine is up to; see `kernel` below
log=$tmp/log
journal=$tmp/journal # what the root helper says, which on a real machine is logged
conf=$tmp/tw-bluetooth.conf
mkdir -p "$bin" "$rootbin"

fail() {
	echo "FAIL: $*" >&2
	[ -s "$log" ] && sed 's/^/  log: /' "$log" >&2
	[ -s "$journal" ] && sed 's/^/  journal: /' "$journal" >&2
	[ -s "$tmp/out" ] && sed 's/^/  out: /' "$tmp/out" >&2
	exit 1
}
calls() { grep -c "$1" "$log" || true; } # log lines matching
said() { grep -q "$1" "$tmp/out"; }
logged() { grep -q "$1" "$journal" 2>/dev/null; }

# --- The made-up machine ------------------------------------------------------
# A laptop with one Bluetooth adapter on USB, driven by btusb with btintel
# under it, and a Wi-Fi card whose radio is none of tw-bluetooth's business.
# In $state:
#   driver          another driver than btusb drives the adapter: its name
#   daemon          the Bluetooth service runs, and has rfcomm and bnep open
#   timer           the timer that switches Bluetooth off is running
#   remembered      what systemd remembers of the radio's block: 1 is blocked
#   restore         systemd has yet to put that back on the adapter just found
#   restore-early   it does so before the helper has looked, not after
#   socket          a program has Bluetooth open: the core cannot be unloaded
#   socket-closes   ... and lets go a moment later
#   no-adapter      the driver loads and finds nothing
#   driver-fails    the driver does not load
#   denied          polkit does not let the user start the units
cat >"$rootbin/kernel" <<EOF
#!/bin/sh
module=$sys/module
driver=\$(cat $state/driver 2>/dev/null || echo btusb)
case \$1 in
load) # the driver, and what the kernel and systemd do when it finds the adapter
	mkdir -p \$module/bluetooth/holders \$module/btintel/holders \$module/\$driver/holders \\
		$sys/class/bluetooth
	ln -sfn ../../\$driver \$module/bluetooth/holders/\$driver
	ln -sfn ../../btintel \$module/bluetooth/holders/btintel
	ln -sfn ../../\$driver \$module/btintel/holders/\$driver
	[ -e $state/no-adapter ] && exit 0
	mkdir -p $sys/class/bluetooth/hci0 $sys/class/rfkill/rfkill1
	echo bluetooth >$sys/class/rfkill/rfkill1/type
	echo 0 >$sys/class/rfkill/rfkill1/soft # a new radio is not blocked
	cp $state/remembered $state/restore
	"\$0" start # systemd wants the service for the adapter
	;;
start) # systemd is asked to start the service, and does if it may: the
	# Bluetooth core has to be loaded, which is the unit's own condition,
	# and with tw's line added to the unit the helper's note has to be
	# there.
	[ -d $sys/class/bluetooth ] || exit 0
	if grep -q '^ConditionPathExists=/run/tw-bluetooth\$' "$data/tw-bluetooth-service.conf" 2>/dev/null; then
		[ -e $run/tw-bluetooth ] || exit 0
	fi
	exec "\$0" daemon
	;;
daemon) # the service runs. It opens the core, which the kernel loads for it
	# if need be, and rfcomm and bnep.
	: >$state/daemon
	mkdir -p \$module/bluetooth/holders \$module/rfcomm/holders \$module/bnep/holders \\
		$sys/class/bluetooth
	ln -sfn ../../rfcomm \$module/bluetooth/holders/rfcomm
	ln -sfn ../../bnep \$module/bluetooth/holders/bnep
	;;
unload)
	[ -d "\$module/\$2" ] || exit 1
	[ -z "\$(ls "\$module/\$2/holders")" ] || exit 1 # another module needs it
	case \$2 in
	rfcomm | bnep) [ ! -e $state/daemon ] || exit 1 ;;
	bluetooth) [ ! -e $state/daemon ] && [ ! -e $state/socket ] || exit 1 ;;
	esac
	rm -rf "\$module/\$2"
	rm -f \$module/*/holders/"\$2"
	if [ "\$2" = "\$driver" ]; then
		# The adapter goes with its driver, and so does what only the
		# driver needed.
		rm -rf $sys/class/bluetooth/hci0* $sys/class/rfkill/rfkill1 $state/restore
		"\$0" unload btintel
		"\$0" unload bluetooth
	fi
	if [ "\$2" = bluetooth ]; then
		rmdir $sys/class/bluetooth
	fi
	exit 0
	;;
restore) # systemd puts the radio's block back
	[ -e $state/restore ] || exit 0
	cat $state/restore >$sys/class/rfkill/rfkill1/soft
	rm $state/restore
	;;
esac
EOF
cat >"$rootbin/modprobe" <<EOF
#!/bin/sh
echo "modprobe \$*" >>"$log"
[ "\$1" = -r ] && exec "$rootbin/kernel" unload "\$2"
if [ "\$1" = "\$(cat "$state/driver" 2>/dev/null || echo btusb)" ]; then
	[ -e "$state/driver-fails" ] && exit 1
	exec "$rootbin/kernel" load
fi
echo "modprobe: FATAL: Module \$1 not found" >&2
exit 1
EOF
cat >"$rootbin/systemctl" <<EOF
#!/bin/sh
echo "root systemctl \$*" >>"$log"
case "\$*" in
"stop bluetooth.service")
	rm -f "$state/daemon"
	# The sound service takes its leave of the service that has just gone,
	# and the system bus starts a service that is called while it is not
	# running. On the real machine that took seven milliseconds.
	"$rootbin/kernel" start
	;;
"restart tw-bluetooth-idle.timer") : >"$state/timer" ;;
"stop tw-bluetooth-idle.timer") rm -f "$state/timer" ;;
esac
EOF
cat >"$rootbin/udevadm" <<EOF
#!/bin/sh
echo "udevadm \$*" >>"$log"
[ -e "$state/restore-early" ] && "$rootbin/kernel" restore
exit 0
EOF
# No test waits for real: a sleep is the moment in which the things the
# helper waits for happen.
cat >"$rootbin/sleep" <<EOF
#!/bin/sh
echo "sleep \$*" >>"$log"
"$rootbin/kernel" restore
if [ -e "$state/socket-closes" ]; then
	rm -f "$state/socket" "$state/socket-closes"
fi
EOF
# What tw-bluetooth reaches the root helper through: the unit, which runs
# the helper as root would, with the file of drivers the unit names.
cat >"$bin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >>"$log"
case "\$*" in
"start tw-bluetooth@on.service") word=on ;;
"start tw-bluetooth@off.service") word=off ;;
*) exit 0 ;;
esac
[ -e "$state/denied" ] && exit 1
PATH="$rootbin:\$PATH" TW_SYS="$sys" TW_RUN="$run" TW_BLUETOOTH_CONF="$conf" \\
	"$data/tw-bluetooth-root" "\$word" 2>>"$journal"
EOF
# The drivers are on the disk for the running kernel, unless a test says a
# kernel update has taken them away.
printf '#!/bin/sh\n[ ! -e "%s/no-modules" ]\n' "$state" >"$rootbin/modinfo"
chmod +x "$rootbin"/* "$bin"/*

machine() { # the laptop as it boots with tw installed: no driver, no adapter
	rm -rf "$sys" "$run" "$state" "$journal" "$tmp/out"
	mkdir -p "$sys/module" "$sys/class/rfkill/rfkill0" "$run" "$state"
	echo wlan >"$sys/class/rfkill/rfkill0/type"
	echo 1 >"$sys/class/rfkill/rfkill0/soft"
	echo 1 >"$state/remembered" # blocked when the adapter was last there
	cp "$data/tw-bluetooth.conf" "$conf"
	: >"$log"
}
# The same laptop before its first reboot with tw's files: the driver loaded
# as it started, the service was started before tw's line was added to its
# unit, and systemd blocked the radio again.
machine_on_from_boot() {
	machine
	"$rootbin/kernel" load
	"$rootbin/kernel" daemon
	"$rootbin/kernel" restore
}

# tw-bluetooth as the person at the computer runs it; what it prints is kept.
bluetooth() {
	PATH="$bin:$PATH" TW_SYS="$sys" TW_RUN="$run" "$data/tw-bluetooth" "$@" >"$tmp/out" 2>&1
}
# The root helper as the timer's unit runs it.
root() {
	PATH="$rootbin:$PATH" TW_SYS="$sys" TW_RUN="$run" TW_BLUETOOTH_CONF="$conf" \
		"$data/tw-bluetooth-root" "$@" 2>>"$journal"
}

loaded() { [ -d "$sys/module/$1" ]; }
adapter() { [ -d "$sys/class/bluetooth/hci0" ]; }
daemon() { [ -e "$state/daemon" ]; }
timer() { [ -e "$state/timer" ]; }
flagged() { [ -e "$run/tw-bluetooth" ]; }
blocked() { [ "$(cat "$sys/class/rfkill/rfkill1/soft")" != 0 ]; }
connect() { mkdir "$sys/class/bluetooth/hci0:$1"; }
disconnect() { rmdir "$sys/class/bluetooth/hci0:$1"; }
# Nothing of Bluetooth is left: no module, adapter, service, timer or note.
all_off() {
	[ -z "$(ls "$sys/module")" ] && ! adapter && ! daemon && ! timer && ! flagged
}
# On as `tw-bluetooth on` leaves it.
all_on() {
	loaded btusb && adapter && daemon && timer && flagged && ! blocked
}

# --- Off as the machine starts, on when asked ---------------------------------
machine
bluetooth || fail "status failed"
said '^Bluetooth: off$' || fail "status of a machine that has just started"
bluetooth on || fail "on failed"
all_on || fail "on did not leave Bluetooth on, unblocked, with the timer running"
said 'Bluetooth is on: connect with bluetoothctl' || fail "on does not say what to do next"
said 'ten minutes' || fail "on does not say that Bluetooth goes off by itself"
[ "$(calls '^modprobe btusb$')" = 1 ] || fail "on did not load the driver once"
[ "$(calls '^udevadm settle')" = 1 ] || fail "on did not let udev finish"
[ "$(cat "$sys/class/rfkill/rfkill0/soft")" = 1 ] || fail "on unblocked the Wi-Fi radio"
bluetooth status
said '^Bluetooth: on, nothing connected. It goes off when that has lasted ten minutes.$' ||
	fail "status after on"
said 'blocked' && fail "status calls an unblocked radio blocked"

# systemd put the block back after the helper had lifted it (above), or
# before; or it remembers the radio as not blocked. On either way.
machine
: >"$state/restore-early"
bluetooth on || fail "on failed"
all_on || fail "on left the radio blocked when systemd put the block back first"
machine
echo 0 >"$state/remembered"
bluetooth on || fail "on failed"
all_on || fail "on failed with a radio that was not blocked"

# A second `on` changes nothing but the timer, which starts over.
bluetooth on || fail "a second on failed"
all_on || fail "a second on did not leave Bluetooth on"
[ "$(calls 'restart tw-bluetooth-idle.timer')" = 2 ] || fail "a second on did not start the timer over"
# The service is started too: an adapter that was there already brings no
# event that would start it.
[ "$(calls 'start --no-block bluetooth.service')" = 2 ] || fail "on did not start the Bluetooth service"

# --- Off by itself ------------------------------------------------------------
# The timer ran out while an `on` was running: the `on` that just finished
# stands.
root idle || fail "idle failed right after on"
all_on || fail "idle undid an on that had just finished"
# From here on the `on` is long past.
touch -d '-11 minutes' "$run/tw-bluetooth"
# The timer ran out with something connected: nothing happens.
connect 256
bluetooth status
said '^Bluetooth: on, 1 device connected.$' || fail "status with a device connected"
root idle || fail "idle failed with a device connected"
all_on || fail "Bluetooth went off with a device connected"
connect 257
bluetooth status
said '^Bluetooth: on, 2 devices connected.$' || fail "status with two devices connected"
disconnect 256
root idle
all_on || fail "Bluetooth went off with one of two devices still connected"
# After a kernel update the drivers are gone from the disk: unloaded now,
# Bluetooth could not come back until a reboot, so it stays on.
disconnect 257
: >"$state/no-modules"
root idle || fail "idle failed with the drivers gone from the disk"
all_on || fail "Bluetooth went off although its drivers could not be loaded again"
rm "$state/no-modules"
# ... and with nothing connected: off, the service stopped before the
# modules it has open.
: >"$log"
root idle || fail "idle failed with nothing connected"
all_off || fail "Bluetooth stayed on with nothing connected"
[ "$(grep -n 'stop bluetooth.service' "$log" | head -n 1 | cut -d: -f1)" -lt \
	"$(grep -n 'modprobe -r' "$log" | head -n 1 | cut -d: -f1)" ] ||
	fail "the modules were unloaded before the service was stopped"
bluetooth status
said '^Bluetooth: off$' || fail "status after Bluetooth went off by itself"

# A machine where Bluetooth is on from boot is left alone by the timer, and
# says so; `off` works there too.
machine_on_from_boot
root idle || fail "idle failed on a machine with Bluetooth on from boot"
loaded btusb && adapter && daemon || fail "idle switched off a Bluetooth that tw-bluetooth had not switched on"
bluetooth status
said "^Bluetooth: on since the machine started, nothing connected. It stays on until 'tw-bluetooth off'.$" ||
	fail "status of Bluetooth on from boot"
said '^Its radio is blocked: rfkill unblock bluetooth$' || fail "status does not say that the radio is blocked"
bluetooth off || fail "off failed on a machine with Bluetooth on from boot"
all_off || fail "off left something of a Bluetooth that was on from boot"
said '^Bluetooth is off.$' || fail "off does not say so"

# What the first switch-off on the real machine left, before the service
# was kept from starting again: the driver and the adapter gone, the service
# back, holding the core. Off clears that up.
machine
"$rootbin/kernel" daemon
bluetooth off || fail "off failed with only the service and the core left"
all_off || fail "off left the service or the core it holds"

# --- Off by hand --------------------------------------------------------------
# With a device connected: off all the same.
machine
bluetooth on
connect 256
bluetooth off || fail "off failed with a device connected"
all_off || fail "off left Bluetooth on with a device connected"
# Off already: nothing is unloaded or stopped but the timer.
: >"$log"
bluetooth off || fail "off failed with Bluetooth already off"
[ "$(calls modprobe)" = 0 ] || fail "off unloaded something with Bluetooth already off"
[ "$(calls 'stop bluetooth.service')" = 0 ] || fail "off stopped the service with Bluetooth already off"

# A program still has Bluetooth open after the service has gone, and lets go
# a moment later: off waits for it.
machine
bluetooth on
: >"$state/socket"
: >"$state/socket-closes"
bluetooth off || fail "off failed although the program let go"
all_off || fail "off did not finish after the program let go"
# One that does not let go: off says so and fails, with the adapter gone
# and the core left; nothing is waiting to switch off again. Once the
# program has gone, off finishes the job.
machine
bluetooth on
: >"$state/socket"
bluetooth off && fail "off reported success with the modules still loaded"
said 'could not switch Bluetooth off' || fail "a failed off does not say so"
logged 'a program still has Bluetooth open' || fail "the helper did not say why off failed"
loaded bluetooth || fail "the made-up kernel unloaded a module in use"
adapter && fail "off left the adapter with a program holding the core"
flagged && fail "a failed off left the note that Bluetooth goes off by itself"
timer && fail "a failed off left the timer running"
[ "$(calls '^sleep 0.5$')" = 4 ] || fail "off did not try five times"
bluetooth status
said "^Bluetooth: no adapter, but its modules are loaded. 'tw-bluetooth off' unloads them.$" ||
	fail "status with only the core left"
rm "$state/socket"
bluetooth off || fail "off failed once the program had gone"
all_off || fail "off did not unload the core that was left"

# --- What can go wrong switching on -------------------------------------------
# The file names no driver, or is not there.
machine
echo '# blacklist nothing' >"$conf"
bluetooth on && fail "on reported success with no driver named"
said 'could not switch Bluetooth on' || fail "a failed on does not say so"
logged 'names no driver to load' || fail "the helper did not say that no driver is named"
all_off || fail "a failed on left something behind"
machine
rm "$conf"
bluetooth on && fail "on reported success without the file of drivers"
all_off || fail "a failed on left something behind"
# The driver does not load.
machine
: >"$state/driver-fails"
bluetooth on && fail "on reported success although the driver did not load"
logged 'no Bluetooth adapter appeared after loading: btusb' || fail "the helper did not say that no adapter appeared"
all_off || fail "a driver that did not load left something behind"
# It loads and finds no adapter: unloaded again.
machine
: >"$state/no-adapter"
bluetooth on && fail "on reported success with no adapter"
[ "$(calls '^modprobe btusb$')" = 1 ] || fail "the driver was not tried"
all_off || fail "a driver that found no adapter was left loaded"
# The file also names a driver this machine does not have: on all the same.
machine
printf '%s\n' 'blacklist hci_uart' '  blacklist   btusb  # the one in use' >"$conf"
bluetooth on || fail "on failed because of a driver this machine does not have"
all_on || fail "on with a second, unknown driver did not leave Bluetooth on"
[ "$(calls '^modprobe hci_uart$')" = 1 ] || fail "the second driver was not tried"
# An adapter whose driver the file does not name (hci_uart, on a bus of its
# own) is there from boot. Once off has unloaded that driver, on loads it
# again: the helper remembers what it unloaded.
machine
echo hci_uart >"$state/driver"
"$rootbin/kernel" load
bluetooth off || fail "off failed with an adapter driven by hci_uart"
all_off || fail "off left an adapter driven by hci_uart"
bluetooth on || fail "on could not bring back a driver the file does not name"
loaded hci_uart && adapter && daemon && timer && flagged ||
	fail "on did not leave an adapter driven by hci_uart on"
# The units may not be started.
machine
: >"$state/denied"
bluetooth on && fail "on reported success although the unit was refused"
said 'could not switch Bluetooth on' || fail "a refused on does not say so"
all_off || fail "a refused on changed something"
bluetooth off && fail "off reported success although the unit was refused"

# --- Asked the wrong thing ----------------------------------------------------
status=0
bluetooth sideways || status=$?
[ "$status" = 2 ] || fail "an unknown command exits with $status"
said 'tw-bluetooth on ' && said 'tw-bluetooth off ' || fail "an unknown command does not print the usage"
status=0
root sideways || status=$?
[ "$status" = 2 ] || fail "the helper exits with $status on an unknown word"
[ "$(calls modprobe)" = 0 ] || fail "an unknown word loaded or unloaded something"

# --- The files that go with them ----------------------------------------------
# These name each other, and nothing else checks that they agree.
grep -q '^OnActiveSec=10min$' "$data/tw-bluetooth-idle.timer" && grep -q 'ten minutes' "$data/tw-bluetooth" ||
	fail "the timer and what tw-bluetooth prints no longer name the same time"
grep -q '^Unit=tw-bluetooth@idle.service$' "$data/tw-bluetooth-idle.timer" ||
	fail "the timer does not start the helper's idle unit"
grep -q 'TEST=="/run/tw-bluetooth"' "$data/tw-bluetooth-udev.rules" &&
	grep -q '^flag=${TW_RUN:-/run}/tw-bluetooth$' "$data/tw-bluetooth-root" ||
	fail "the udev rule and the helper no longer name the same note"
grep -q 'restart tw-bluetooth-idle.timer"$' "$data/tw-bluetooth-udev.rules" &&
	grep -q '^timer=tw-bluetooth-idle.timer$' "$data/tw-bluetooth-root" ||
	fail "the udev rule and the helper no longer name the same timer"
for word in on off; do
	grep -q "\"tw-bluetooth@$word.service\"" "$data/tw-bluetooth.rules" &&
		grep -q "systemctl start tw-bluetooth@$word.service" "$data/tw-bluetooth" ||
		fail "the polkit rule and tw-bluetooth no longer name the same unit for '$word'"
done
grep -q '^ConditionPathExists=/run/tw-bluetooth$' "$data/tw-bluetooth-service.conf" ||
	fail "the line added to the Bluetooth service and the helper no longer name the same note"
grep -q '^Environment=TW_BLUETOOTH_CONF=@conf@$' "$data/tw-bluetooth@.service.in" ||
	fail "the unit does not tell the helper where the file of drivers is"
# The udev rules, as far as udev can check them without a device.
if udevadm verify --help >/dev/null 2>&1; then
	udevadm verify --no-style "$data/tw-bluetooth-udev.rules" >"$tmp/out" 2>&1 ||
		fail "udev does not accept the rules"
fi

echo "ok: off at boot, on, off by itself and by hand, a program holding on, failures switching on"
