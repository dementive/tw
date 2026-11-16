#!/bin/sh
# tw-gpu, its root helper and the GPU choice in tw-session, run against a
# made-up machine: directories standing in for /sys, /dev, /proc and /run,
# and stand-ins for systemctl and modprobe that load and unload a pretend
# driver and take a pretend GPU off its bus. Nothing here touches a real GPU
# or driver, or needs root. What it cannot show is that the real driver
# unloads, that the real GPU leaves the bus, powers down and comes back, or
# that a program draws with it afterwards.
# $1 = the data/ directory of the source tree.
set -eu
data=$1
tmp=$(mktemp -d "$PWD/gpu-test.XXXXXX")
kernel_pid=
cleanup() {
	pkill -f "$data/tw-gpu (settle|retry|try)" 2>/dev/null || true
	[ -z "$kernel_pid" ] || kill "$kernel_pid" 2>/dev/null || true
	rm -rf "$tmp"
}
trap cleanup EXIT
sys=$tmp/sys
dev=$tmp/dev
proc=$tmp/proc
run=$tmp/run         # stands in for /run: where the root helper leaves its note
bin=$tmp/bin         # stand-ins for tw-gpu: systemctl
rootbin=$tmp/rootbin # stand-ins for tw-gpu-root: systemctl, modprobe
log=$tmp/log
mkdir -p "$bin" "$rootbin" "$run" "$tmp/user" "$tmp/home"
# The driver is on the disk for the running kernel, unless a test says a
# kernel update has taken it away.
for dir in "$bin" "$rootbin"; do
	printf '#!/bin/sh\n[ ! -e "%s/no-modules" ]\n' "$tmp" >"$dir/modinfo"
	chmod +x "$dir/modinfo"
done
: >"$log"

fail() {
	echo "FAIL: $*" >&2
	sed 's/^/  log: /' "$log" >&2
	[ -e "$tmp/report" ] && sed 's/^/  report: /' "$tmp/report" >&2
	exit 1
}
wait_until() { # <description> <command...>: poll for up to 5 seconds
	description=$1
	shift
	tries=0
	while ! "$@" >/dev/null 2>&1; do
		tries=$((tries + 1))
		[ "$tries" -le 100 ] || fail "timed out waiting for $description"
		sleep 0.05
	done
}
calls() { grep -c "$1" "$log" || true; } # log lines matching
last_report() { tail -n 1 "$tmp/report"; }
expect_report() { # <description> <regex>
	last_report | grep -Eq "$2" || fail "$1: $(last_report)"
}

# --- The made-up machine ------------------------------------------------------
# A laptop: an Intel GPU with the built-in panel, and behind a port of its
# own an NVIDIA GPU with an HDMI output and a sound function.
devices=$sys/devices/pci0000:00
intel=$devices/0000:00:02.0
port=$devices/0000:00:01.0
nvidia=$port/0000:01:00.0
bus=$sys/bus/pci/devices
flag=$run/tw-gpu-off

on_bus() { [ -e "$bus/0000:01:00.0" ]; }
loaded() { [ -d "$sys/module/nvidia" ]; }
off() { ! on_bus && [ -e "$flag" ]; } # switched off, as the root helper leaves it
# Nothing of tw-gpu is waiting to switch off any more.
settled() { ! pgrep -f "$data/tw-gpu (settle|retry|try)" >/dev/null; }

machine() { # the laptop as it boots: everything on the bus, driver loaded
	rm -rf "$sys" "$dev" "$proc" "$flag" "$tmp/unplugged"
	mkdir -p "$sys/class/drm" "$sys/module" "$dev/dri" "$bus" "$proc" \
		"$sys/bus/pci/drivers/i915" "$sys/bus/pci/drivers/nvidia" \
		"$intel/drm/card1" "$intel/drm/renderD128" "$sys/class/drm/card1-eDP-1" \
		"$nvidia/power" "$port/0000:01:00.1"
	echo 0x8086 >"$intel/vendor"
	echo 0x030000 >"$intel/class"
	echo 1 >"$intel/boot_vga"
	ln -s ../../../bus/pci/drivers/i915 "$intel/driver"
	ln -s "$intel" "$intel/drm/card1/device"
	ln -s "$intel/drm/card1" "$sys/class/drm/card1"
	echo connected >"$sys/class/drm/card1-eDP-1/status"
	: >"$dev/dri/card1"
	: >"$dev/dri/renderD128"
	echo 0x8086 >"$port/vendor"
	echo 0x060400 >"$port/class"
	echo D0 >"$port/power_state"
	: >"$port/rescan"
	: >"$sys/bus/pci/rescan"
	echo 0x10de >"$nvidia/vendor"
	echo 0x030000 >"$nvidia/class"
	echo 0 >"$nvidia/boot_vga"
	echo D0 >"$nvidia/power_state"
	echo 0x10de >"$port/0000:01:00.1/vendor" # the sound function: not a GPU
	echo 0x040300 >"$port/0000:01:00.1/class"
	ln -s "$intel" "$bus/0000:00:02.0"
	ln -s "$port" "$bus/0000:00:01.0"
	ln -s "$nvidia" "$bus/0000:01:00.0"
	ln -s "$port/0000:01:00.1" "$bus/0000:01:00.1"
	"$bin/driver" load
}

# driver load|unload: what the real driver does to /sys and /dev.
cat >"$bin/driver" <<EOF
#!/bin/sh
case \$1 in
load)
	[ -d "$nvidia" ] || exit 1 # no such GPU on the bus: the driver refuses
	mkdir -p "$sys/module/nvidia" "$sys/module/nvidia_modeset" "$sys/module/nvidia_drm" \\
		"$nvidia/drm/card0" "$nvidia/drm/renderD129" "$sys/class/drm/card0-HDMI-A-1"
	echo 2 >"$sys/module/nvidia/refcnt"
	echo 1 >"$sys/module/nvidia_modeset/refcnt"
	echo 0 >"$sys/module/nvidia_drm/refcnt"
	ln -sfn ../../../../bus/pci/drivers/nvidia "$nvidia/driver"
	ln -sfn "$nvidia" "$nvidia/drm/card0/device"
	ln -sfn "$nvidia/drm/card0" "$sys/class/drm/card0"
	[ -e "$tmp/hdmi" ] && status=connected || status=disconnected
	echo \$status >"$sys/class/drm/card0-HDMI-A-1/status"
	: >"$dev/dri/card0"
	: >"$dev/dri/renderD129"
	echo suspended >"$nvidia/power/runtime_status"
	;;
unload)
	rm -rf "$sys"/module/nvidia* "$nvidia/drm" "$nvidia/driver" "$sys/class/drm/card0" \\
		"$sys/class/drm/card0-HDMI-A-1" "$dev/dri/card0" "$dev/dri/renderD129"
	;;
esac
EOF
# pci remove|rescan: what the kernel does when the GPU is taken off the bus,
# and when the port above it is scanned again.
cat >"$bin/pci" <<EOF
#!/bin/sh
case \$1 in
remove)
	rm -f "$bus/0000:01:00.0"
	mv "$nvidia" "$tmp/unplugged"
	echo D3cold >"$port/power_state"
	;;
rescan)
	[ ! -d "$tmp/unplugged" ] || mv "$tmp/unplugged" "$nvidia"
	rm -f "$nvidia/remove"
	ln -sfn "$nvidia" "$bus/0000:01:00.0"
	echo D0 >"$port/power_state"
	;;
esac
EOF
# What tw-gpu reaches the root helper through; this does the helper's work.
cat >"$bin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >>"$log"
case "\$*" in
"start tw-gpu@on.service")
	[ -e "$tmp/on-fails" ] && exit 1
	if [ -e "$flag" ]; then
		"$bin/pci" rescan
		rm "$flag"
	fi
	"$bin/driver" load
	;;
"start tw-gpu@off.service")
	if [ ! -e "$tmp/held" ] && [ -e "$bus/0000:01:00.0" ]; then
		"$bin/driver" unload
		echo "$port" >"$flag"
		"$bin/pci" remove
	fi
	;;
esac
EOF
# A program that says what it was started with.
cat >"$bin/report" <<EOF
#!/bin/sh
echo "offload=\${__NV_PRIME_RENDER_OFFLOAD-} vulkan=\${__VK_LAYER_NV_optimus-}" \\
	"glx=\${__GLX_VENDOR_LIBRARY_NAME-} egl=\${__EGL_VENDOR_LIBRARY_FILENAMES-}" \\
	"icd=\${VK_LOADER_DRIVERS_DISABLE-} driver=\$([ -d "$sys/module/nvidia" ] && echo loaded || echo absent)" \\
	"args=\$*" >>"$tmp/report"
EOF
chmod +x "$bin/driver" "$bin/pci" "$bin/systemctl" "$bin/report"

# Something that was not started with tw-gpu has the GPU open, as btop does,
# or the login screen for a few seconds after login; and it lets go again.
hold() {
	: >"$tmp/held"
	echo 13 >"$sys/module/nvidia/refcnt"
}
release() {
	rm -f "$tmp/held"
	echo 2 >"$sys/module/nvidia/refcnt"
}

# tw-gpu as a program in the session would run it: the session's two
# variables that keep programs on the Intel GPU are set. The launcher's
# variables are not, even if this test was started from a launched program.
gpu() {
	env -u DESKTOP_ENTRY_ID -u FUZZEL_DESKTOP_FILE_ID \
		PATH="$bin:$PATH" TW_SYS="$sys" TW_DEV="$dev" TW_RUN="$run" HOME="$tmp/home" \
		XDG_RUNTIME_DIR="$tmp/user" XDG_DATA_DIRS="$tmp/share" \
		WLR_DRM_DEVICES=/dev/dri/card1 TW_GPU_PAUSE=0.2 \
		__EGL_VENDOR_LIBRARY_FILENAMES=/mesa.json VK_LOADER_DRIVERS_DISABLE='*nvidia*' \
		"$@"
}
on_nvidia='offload=1 vulkan=NVIDIA_only glx=nvidia egl= icd= driver=loaded'
on_intel='offload= vulkan= glx= egl=/mesa.json icd=[*]nvidia[*]' # a regular expression

# --- tw-gpu: status and off ---------------------------------------------------
machine
[ "$(gpu "$data/tw-gpu" status)" = "NVIDIA GPU 0000:01:00.0: on, asleep" ] ||
	fail "status with the driver loaded: $(gpu "$data/tw-gpu" status)"
gpu "$data/tw-gpu" off || fail "off failed on an idle GPU"
off || fail "off left the GPU on the bus"
loaded && fail "off left the driver loaded"
[ "$(calls 'start tw-gpu@off')" = 1 ] || fail "off did not start the unit once"
[ "$(gpu "$data/tw-gpu" status)" = "NVIDIA GPU: off, powered down" ] ||
	fail "status with the GPU off: $(gpu "$data/tw-gpu" status)"
echo D0 >"$port/power_state"
gpu "$data/tw-gpu" status | grep -q 'off, but its slot is still powered (D0)' ||
	fail "status does not say that the GPU failed to power down"
gpu "$data/tw-gpu" off || fail "off failed with the GPU already off"
[ "$(calls 'start tw-gpu@off')" = 1 ] || fail "off did something with the GPU already off"
# On the bus with no driver: neither on nor off.
"$bin/pci" rescan
rm "$flag"
gpu "$data/tw-gpu" status | grep -q 'on the bus without its driver (D0)' ||
	fail "status of a GPU without its driver: $(gpu "$data/tw-gpu" status)"
gpu "$data/tw-gpu" off || fail "off failed on a GPU without its driver"
off || fail "off left a GPU without its driver on the bus"

# --- run: on for the program, off after it ------------------------------------
before=$(calls 'start tw-gpu@on')
gpu "$data/tw-gpu" run report a 'b c' || fail "run failed"
expect_report "a program started with run" "^$on_nvidia args=a b c\$"
[ "$(calls 'start tw-gpu@on')" = $((before + 1)) ] || fail "run did not switch the GPU on once"
wait_until "the GPU to go off after the program" settled
off || fail "the GPU stayed on after its only program exited"
status=0
gpu "$data/tw-gpu" run sh -c 'exit 7' || status=$?
[ "$status" = 7 ] || fail "run returned $status for a program that exited with 7"
wait_until "the switch-off to finish" settled

# A program whose terminal is closed: everything in the terminal's process
# group is hung up on, and the GPU still goes off. (On the real machine it
# once did not, because the part that switches off was hung up on too.)
(gpu setsid "$data/tw-gpu" run sleep 31.7 2>/dev/null &)
wait_until "the program to start" pgrep -xf 'sleep 31.7'
loaded || fail "the GPU is not on for a running program"
kill -HUP -- "-$(pgrep -xf 'sleep 31.7')"
wait_until "the GPU to go off after the terminal was closed" settled
off || fail "the GPU stayed on after its program's terminal was closed"

# Two programs: the GPU stays on until the second one is gone, and is
# switched off once.
before=$(calls 'start tw-gpu@off')
gpu "$data/tw-gpu" run sleep 0.6 &
gpu "$data/tw-gpu" run sleep 1.8 &
sleep 1.2
loaded || fail "the GPU went off while a second program was using it"
[ "$(calls 'start tw-gpu@off')" = "$before" ] || fail "switch-off attempted with a program running"
# Nor can it be switched off by hand meanwhile.
if gpu "$data/tw-gpu" off 2>/dev/null; then fail "off succeeded with a program running"; fi
loaded || fail "off by hand took the GPU from a running program"
wait
wait_until "the GPU to go off after both programs" settled
off || fail "the GPU stayed on after both programs exited"
[ "$(calls 'start tw-gpu@off')" = $((before + 1)) ] || fail "expected one switch-off for two programs"

# A program that leaves something running and returns: run returns with it,
# and the GPU stays on for what was left behind.
gpu "$data/tw-gpu" run sh -c 'sleep 1.5 &'
loaded || fail "the GPU went off under a program left in the background"
sleep 0.5
loaded || fail "the GPU went off under a program left in the background"
wait_until "the GPU to go off after the background program" settled
off || fail "the GPU stayed on after the background program exited"

# --- on: stays on until off -----------------------------------------------------
gpu "$data/tw-gpu" on || fail "on failed"
loaded || fail "on did not load the driver"
gpu "$data/tw-gpu" status | grep -q "kept on by 'tw-gpu on'" || fail "status does not say why it is on"
gpu "$data/tw-gpu" run report kept
wait_until "the switch-off check to finish" settled
loaded || fail "a program exiting switched off a GPU that was asked to stay on"
gpu "$data/tw-gpu" off || fail "off failed after on"
off || fail "off after on left the GPU on"

# --- Something else is using the GPU: it goes off when that has let go -----------
# The owner's case: the program started with tw-gpu exits while btop still
# has the GPU open. It must not stay on for good.
gpu "$data/tw-gpu" on
rm "$tmp/user/tw-gpu.keep"
hold
gpu "$data/tw-gpu" run report beside-btop
sleep 0.6
loaded || fail "the GPU was taken from the program still using it"
settled && fail "nothing is waiting for the GPU to become free"
before=$(calls 'start tw-gpu@off')
sleep 0.6
[ "$(calls 'start tw-gpu@off')" = "$before" ] || fail "kept trying to switch off although nothing had changed"
release
wait_until "the GPU to go off once the other program let go" off
wait_until "the waiting to end" settled
# The same when switching off by hand, which says what will happen.
gpu "$data/tw-gpu" on
hold
if gpu "$data/tw-gpu" off 2>"$tmp/err"; then fail "off claimed success on a GPU in use"; fi
grep -q 'goes off when' "$tmp/err" || fail "off did not say that the GPU goes off later"
loaded || fail "the driver went although the GPU was in use"
release
wait_until "the GPU to go off after a refused off" off
wait_until "the waiting to end" settled
# Asked to stay on while that wait is pending: the wait ends, the GPU stays.
gpu "$data/tw-gpu" on
hold
if gpu "$data/tw-gpu" off 2>/dev/null; then fail "off claimed success on a GPU in use"; fi
gpu "$data/tw-gpu" on
wait_until "the waiting to end after on" settled
release
sleep 0.5
loaded || fail "a pending switch-off overrode 'tw-gpu on'"
# One that tw drives a monitor with is not even tried.
before=$(calls 'start tw-gpu@off')
if gpu env WLR_DRM_DEVICES=/dev/dri/card1:/dev/dri/card0 "$data/tw-gpu" off 2>/dev/null; then
	fail "off succeeded on the GPU driving a monitor"
fi
wait_until "the waiting to end for a GPU driving a monitor" settled
[ "$(calls 'start tw-gpu@off')" = "$before" ] || fail "tried to switch off a GPU driving a monitor"
gpu env WLR_DRM_DEVICES=/dev/dri/card1:/dev/dri/card0 "$data/tw-gpu" status |
	grep -q 'drives a monitor' || fail "status does not mention the monitor"
gpu "$data/tw-gpu" off || fail "off failed once the GPU was free"

# --- login: what tw-session runs once tw is up ------------------------------------
# The login screen still has the GPU open. (On the real machine one try was
# not enough, and the GPU stayed on all session.)
"$bin/pci" rescan
rm "$flag"
"$bin/driver" load
hold
gpu "$data/tw-gpu" login || fail "login failed while the login screen had the GPU"
loaded || fail "login took the GPU from the login screen"
sleep 0.5
release
wait_until "login to switch the GPU off once the login screen let go" off
wait_until "the waiting to end" settled
# Nothing holds it: off at once.
"$bin/pci" rescan
rm "$flag"
"$bin/driver" load
gpu "$data/tw-gpu" login || fail "login failed on a free GPU"
off || fail "login left a free GPU on"
# Driving a monitor, or asked to stay on: login leaves it alone.
gpu "$data/tw-gpu" on
rm "$tmp/user/tw-gpu.keep"
before=$(calls 'start tw-gpu@off')
gpu env WLR_DRM_DEVICES=/dev/dri/card1:/dev/dri/card0 "$data/tw-gpu" login ||
	fail "login failed on a GPU driving a monitor"
gpu "$data/tw-gpu" on
gpu "$data/tw-gpu" login || fail "login failed on a GPU that was asked to stay on"
wait_until "the waiting to end" settled
loaded || fail "login switched off a GPU it should have left alone"
[ "$(calls 'start tw-gpu@off')" = "$before" ] || fail "login tried to switch off a GPU it should have left alone"
gpu "$data/tw-gpu" off || fail "off failed after login"

# With no GPUs named, tw opened them all (one GPU, or TW_ALL_GPUS=1): the
# GPU stays on after the program, and nothing is left waiting for it.
gpu env -u WLR_DRM_DEVICES "$data/tw-gpu" run report all-gpus
wait_until "the switch-off check to finish" settled
loaded || fail "a GPU that tw has open was switched off"
wait_until "off after a run with every GPU open" gpu "$data/tw-gpu" off

# --- The GPU cannot be switched on: the program still runs -----------------------
: >"$tmp/on-fails"
status=0
gpu "$data/tw-gpu" run sh -c 'report fallback; exit 3' 2>"$tmp/err" || status=$?
[ "$status" = 3 ] || fail "run returned $status when the GPU could not be switched on"
expect_report "a program whose GPU could not be switched on" "^$on_intel driver=absent args=fallback\$"
grep -q 'could not switch' "$tmp/err" || fail "run did not say the GPU could not be switched on"
if gpu "$data/tw-gpu" on 2>/dev/null; then fail "on claimed success"; fi
[ ! -e "$tmp/user/tw-gpu.keep" ] || fail "a failed on left the GPU marked as kept on"
rm "$tmp/on-fails"
wait_until "the switch-off check to finish" settled

# --- launch: only desktop entries that ask for the discrete GPU ------------------
mkdir -p "$tmp/share/applications" "$tmp/home/.local/share/applications"
printf '[Desktop Entry]\nName=Game\nPrefersNonDefaultGPU=true\n' >"$tmp/share/applications/game.desktop"
printf '[Desktop Entry]\nName=Old game\nX-KDE-RunOnDiscreteGpu=true\n' >"$tmp/share/applications/kde.desktop"
printf '[Desktop Entry]\nName=Editor\n' >"$tmp/share/applications/editor.desktop"
printf '[Desktop Entry]\nName=Notes\nPrefersNonDefaultGPU=false\n' >"$tmp/share/applications/notes.desktop"
launch() { # <desktop entry id>: start `report` the way the launcher does
	gpu env DESKTOP_ENTRY_ID="$1" "$data/tw-gpu" launch report "$1"
	wait_until "the switch-off after $1" settled
}
before=$(calls 'start tw-gpu@on')
launch game.desktop
expect_report "an entry that prefers the discrete GPU" "^$on_nvidia args=game.desktop\$"
launch kde.desktop
expect_report "an entry with KDE's key" "^$on_nvidia args=kde.desktop\$"
[ "$(calls 'start tw-gpu@on')" = $((before + 2)) ] || fail "launch did not switch the GPU on for each"
off || fail "the GPU stayed on after the launched program exited"
before=$(calls systemctl)
for entry in editor.desktop notes.desktop missing.desktop ''; do
	launch "$entry"
	expect_report "an entry that does not ask for the GPU ($entry)" "^$on_intel driver=absent args=$entry\$"
done
[ "$(calls systemctl)" = "$before" ] || fail "launch touched the GPU for an ordinary program"
# The user's own copy of an entry is the one that counts.
printf '[Desktop Entry]\nName=Editor\nPrefersNonDefaultGPU=true\n' >"$tmp/home/.local/share/applications/editor.desktop"
launch editor.desktop
expect_report "an entry the user changed" "^$on_nvidia args=editor.desktop\$"
# fuzzel's older name for the same variable.
gpu env FUZZEL_DESKTOP_FILE_ID=game.desktop "$data/tw-gpu" launch report old-name
expect_report "the entry named in FUZZEL_DESKTOP_FILE_ID" "^$on_nvidia args=old-name\$"
wait_until "the switch-off to finish" settled

# --- A machine with no NVIDIA GPU: everything just runs the program --------------
rm -rf "$tmp/unplugged" "$flag"
before=$(calls systemctl)
gpu "$data/tw-gpu" run report plain
expect_report "run without an NVIDIA GPU" "^$on_intel driver=absent args=plain\$"
gpu env DESKTOP_ENTRY_ID=game.desktop "$data/tw-gpu" launch report plain-launch
expect_report "launch without an NVIDIA GPU" "^$on_intel driver=absent args=plain-launch\$"
[ "$(gpu "$data/tw-gpu" status)" = "no NVIDIA GPU" ] || fail "status without an NVIDIA GPU"
if gpu "$data/tw-gpu" on 2>/dev/null; then fail "on succeeded without an NVIDIA GPU"; fi
gpu "$data/tw-gpu" off || fail "off failed without an NVIDIA GPU"
gpu "$data/tw-gpu" login || fail "login failed without an NVIDIA GPU"
[ "$(calls systemctl)" = "$before" ] || fail "something was started on a machine with no NVIDIA GPU"

# --- The root helper ---------------------------------------------------------------
# Like the real one where it matters: removing a module also removes what
# only that module needed, and a name that is no longer loaded is an error
# (the real one is not always that strict; the helper must not rely on it).
cat >"$rootbin/modprobe" <<EOF
#!/bin/sh
echo "modprobe \$*" >>"$log"
case \$1 in
nvidia_drm)
	# $tmp/colliding: udev's own load of the driver is in the way, for as
	# many tries as the number in it.
	if [ -s "$tmp/colliding" ]; then
		left=\$((\$(cat "$tmp/colliding") - 1))
		[ "\$left" -gt 0 ] && echo "\$left" >"$tmp/colliding" || rm "$tmp/colliding"
		exit 1
	fi
	"$bin/driver" load
	;;
-r)
	[ -d "$sys/module/\$2" ] || exit 1
	case \$2 in
	nvidia_drm)
		[ -e "$tmp/drawing" ] && exit 1
		rm -rf "$sys/module/nvidia_drm"
		;;
	nvidia_uvm)
		[ -e "$tmp/computing" ] && exit 1
		rm -rf "$sys/module/nvidia_uvm"
		;;
	nvidia_modeset)
		rm -rf "$sys/module/nvidia_modeset"
		[ -e "$tmp/computing" ] || [ -d "$sys/module/nvidia_uvm" ] || "$bin/driver" unload
		;;
	nvidia)
		[ -e "$tmp/computing" ] && exit 1
		"$bin/driver" unload
		;;
	esac
	;;
esac
EOF
cat >"$rootbin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >>"$log"
case "\$*" in
"is-enabled --quiet nvidia-powerd.service") [ ! -e "$tmp/powerd-disabled" ] ;;
esac
EOF
printf '#!/bin/sh\necho "udevadm $*" >>"%s"\n' "$log" >"$rootbin/udevadm"
chmod +x "$rootbin/modprobe" "$rootbin/systemctl" "$rootbin/udevadm"
# The kernel's part: act on what the helper writes into sysfs.
kernel() {
	while [ ! -e "$tmp/kernel-stop" ]; do
		if [ -s "$nvidia/remove" ]; then "$bin/pci" remove; fi
		if [ -s "$port/rescan" ] && [ ! -e "$tmp/dead-slot" ]; then
			: >"$port/rescan"
			"$bin/pci" rescan
		fi
		sleep 0.05
	done
}
root() { # tw-gpu-root <on|off>, then what it ran, on one line
	: >"$log"
	env PATH="$rootbin:$PATH" TW_SYS="$sys" TW_PROC="$proc" TW_RUN="$run" \
		"$data/tw-gpu-root" "$@" 2>"$tmp/err" || return
	ran=$(tr '\n' ';' <"$log")
}
powerd_start='systemctl is-enabled --quiet nvidia-powerd.service;systemctl reset-failed nvidia-powerd.service;systemctl start --no-block nvidia-powerd.service;'
load='udevadm settle --timeout=5;modprobe nvidia_drm;'
unload='modprobe -r nvidia_drm;systemctl stop nvidia-powerd.service;modprobe -r nvidia_modeset;'
process() { # <pid> <name> <open file>: a running program with a file open
	mkdir -p "$proc/$1/fd"
	echo "$2" >"$proc/$1/comm"
	ln -s "$3" "$proc/$1/fd/7"
}

machine
kernel &
kernel_pid=$!
process 775 nvidia-powerd /dev/nvidiactl # NVIDIA's daemon: never counts as use
process 900 foot /dev/pts/3

# After a kernel update the driver is no longer on the disk: unloaded now,
# it could not be loaded again until a reboot, so it stays.
: >"$tmp/no-modules"
root off || fail "the helper failed when the driver was not on the disk"
[ -z "$ran" ] && loaded && on_bus || fail "the helper unloaded a driver that could not be loaded again: $ran"
if root on; then fail "the helper switched on with no driver on the disk"; fi
rm "$tmp/no-modules"
# A GPU that another driver drives (nouveau) is not the helper's to take away.
ln -sfn ../../../../bus/pci/drivers/nouveau "$nvidia/driver"
root off || fail "the helper failed on a GPU driven by nouveau"
[ -z "$ran" ] && on_bus || fail "the helper took a GPU driven by nouveau off the bus: $ran"
ln -sfn ../../../../bus/pci/drivers/nvidia "$nvidia/driver"

# Off: driver unloaded, GPU off the bus, and a note of the port above it.
root off || fail "the helper failed to switch the GPU off"
[ "$ran" = "$unload" ] || fail "helper off ran: $ran"
wait_until "the GPU to leave the bus" off
[ "$(cat "$flag")" = "$port" ] || fail "the helper noted $(cat "$flag") as the port to scan"
loaded && fail "helper off left the driver loaded"
root off || fail "the helper failed with the GPU already off"
[ -z "$ran" ] || fail "helper off with the GPU already off ran: $ran"

# On: the port is scanned, the GPU comes back, the driver is loaded.
root on || fail "the helper failed to switch the GPU on"
[ "$ran" = "$load$powerd_start" ] || fail "helper on ran: $ran"
on_bus || fail "helper on did not bring the GPU back on the bus"
loaded || fail "helper on did not load the driver"
[ ! -e "$flag" ] || fail "helper on left its note behind"
# On with the GPU on the bus all along: just the driver.
"$bin/driver" unload
root on || fail "the helper failed to load the driver"
[ "$ran" = "$load$powerd_start" ] || fail "helper on, GPU on the bus, ran: $ran"

# udev's own attempt to load the driver is still in the way for the first
# tries. (On the real machine two of five switch-ons failed on this.)
"$bin/driver" unload
echo 2 >"$tmp/colliding"
root on || fail "the helper gave up on a driver that loads at the third try"
[ "$ran" = "${load}modprobe nvidia_drm;modprobe nvidia_drm;$powerd_start" ] ||
	fail "helper on with a load in the way ran: $ran"
loaded || fail "the helper did not load the driver at the third try"
# One that never loads: an error, after a handful of tries.
"$bin/driver" unload
echo 99 >"$tmp/colliding"
status=0
root on || status=$?
[ "$status" = 1 ] || fail "helper on returned $status for a driver that never loads"
[ "$(grep -c 'modprobe nvidia_drm' "$log")" = 6 ] || fail "helper on tried $(grep -c 'modprobe nvidia_drm' "$log") times"
rm "$tmp/colliding"
root on

# With the computing module loaded as well (a program asked for it earlier).
mkdir "$sys/module/nvidia_uvm"
root off || fail "the helper failed to unload the driver and its computing module"
[ "$ran" = "modprobe -r nvidia_drm;systemctl stop nvidia-powerd.service;modprobe -r nvidia_uvm;modprobe -r nvidia_modeset;" ] ||
	fail "helper off with the computing module ran: $ran"
wait_until "the GPU to leave the bus" off
# Off with no driver loaded: only the bus.
root on
"$bin/driver" unload
root off || fail "the helper failed on a GPU without its driver"
[ -z "$ran" ] || fail "helper off without a driver ran: $ran"
wait_until "a GPU without its driver to leave the bus" off

# A program has the GPU open: nothing at all is disturbed, and that is not
# an error.
root on
process 4242 btop /dev/nvidia0
root off || fail "the helper reported an error for a GPU in use"
[ -z "$ran" ] || fail "helper off on a GPU in use ran: $ran"
grep -q 'in use by btop' "$tmp/err" || fail "the helper did not name the program: $(cat "$tmp/err")"
on_bus && loaded && [ ! -e "$flag" ] || fail "the helper touched a GPU in use"
rm -r "$proc/4242"
# Likewise one that draws with it through the GPU's own device file.
process 4243 game /dev/dri/renderD129
root off
[ -z "$ran" ] && grep -q 'in use by game' "$tmp/err" || fail "helper off on a GPU drawing ran: $ran"
rm -r "$proc/4243"

# What a program drew is still held although the program is gone: wait a
# little, then leave everything as it is.
: >"$tmp/drawing"
root off || fail "the helper reported an error for a GPU still drawing"
[ "$ran" = "modprobe -r nvidia_drm;modprobe -r nvidia_drm;modprobe -r nvidia_drm;modprobe -r nvidia_drm;modprobe -r nvidia_drm;" ] ||
	fail "helper off on a GPU still drawing ran: $ran"
on_bus && [ -d "$sys/module/nvidia_drm" ] || fail "the helper took away a GPU still drawing"
grep -q 'in use' "$tmp/err" || fail "the helper did not say the GPU is in use"
rm "$tmp/drawing"

# Something opens it while it is being unloaded: what was unloaded is put back.
: >"$tmp/computing"
root off || fail "the helper reported an error for a GPU opened meanwhile"
[ "$ran" = "${unload}modprobe -r nvidia;modprobe nvidia_drm;$powerd_start" ] ||
	fail "helper off on a GPU opened meanwhile ran: $ran"
on_bus && [ -d "$sys/module/nvidia_drm" ] && [ ! -e "$flag" ] || fail "the helper did not put the driver back"
rm "$tmp/computing"

# The power daemon is left alone if the owner switched it off.
: >"$tmp/powerd-disabled"
root off
wait_until "the GPU to leave the bus" off
root on
[ "$ran" = "${load}systemctl is-enabled --quiet nvidia-powerd.service;" ] ||
	fail "helper on with the power daemon disabled ran: $ran"
rm "$tmp/powerd-disabled"

# The GPU does not come back: the helper looks everywhere, then says so.
root off
wait_until "the GPU to leave the bus" off
: >"$tmp/dead-slot"
status=0
root on || status=$?
[ "$status" = 1 ] || fail "helper on returned $status for a GPU that did not come back"
[ -s "$sys/bus/pci/rescan" ] || fail "the helper did not scan the whole bus for a GPU that stayed away"
grep -q 'did not come back' "$tmp/err" || fail "the helper did not say that the GPU stayed away"
[ -e "$flag" ] || fail "the helper forgot the GPU it could not bring back"
rm "$tmp/dead-slot"
"$bin/pci" rescan
root on || fail "helper on failed once the GPU was back"
[ ! -e "$flag" ] || fail "the helper kept the note of a GPU that is back"

status=0
root sideways || status=$?
[ "$status" = 2 ] && [ ! -s "$log" ] || fail "the helper acted on a word it does not know"
: >"$tmp/kernel-stop" # and let it finish what it is doing
wait "$kernel_pid"
kernel_pid=

# --- tw-session: which GPUs tw uses -------------------------------------------------
mkdir -p "$tmp/usr/glvnd/egl_vendor.d" "$tmp/usr/vulkan/icd.d"
for file in glvnd/egl_vendor.d/10_nvidia.json glvnd/egl_vendor.d/50_mesa.json \
	vulkan/icd.d/intel_icd.json vulkan/icd.d/intel_hasvk_icd.json vulkan/icd.d/nvidia_icd.json; do
	echo '{}' >"$tmp/usr/$file"
done
mesa_egl=$tmp/usr/glvnd/egl_vendor.d/50_mesa.json
mesa_vulkan='*nvidia*' # by the driver manifests' names
session() { # [VAR=value...]: what tw-session decides, on one line
	env -i PATH="$PATH" HOME="$tmp/home" TW_SYS="$sys" TW_SHARE="$tmp/usr" TW_RUN="$run" \
		TW_SESSION_PRINT=1 "$@" sh "$data/tw-session" | tr '\n' ';'
}
# What tw-session runs once tw is up to switch the second GPU off.
step='command -v tw-gpu >/dev/null 2>&1 && tw-gpu login >/dev/null 2>&1 &'
# The accessibility bus is kept off: said to the programs tw starts, and
# passed on to the ones D-Bus and systemd start, as is the GPU they are
# kept on. What is not passed on is taken back from systemd.
a11y='NO_AT_BRIDGE=1 GTK_A11Y=none'
told_gpu='told to services=WAYLAND_DISPLAY XDG_CURRENT_DESKTOP NO_AT_BRIDGE GTK_A11Y __EGL_VENDOR_LIBRARY_FILENAMES VK_LOADER_DRIVERS_DISABLE;taken back='
told_none='told to services=WAYLAND_DISPLAY XDG_CURRENT_DESKTOP NO_AT_BRIDGE GTK_A11Y;taken back= __EGL_VENDOR_LIBRARY_FILENAMES VK_LOADER_DRIVERS_DISABLE'
expect_session() { # <description> <devices> <egl> <vulkan> <step> [VAR=value...]
	description=$1
	told=$told_none
	[ -z "$3" ] || told=$told_gpu
	want="WLR_DRM_DEVICES=$2;__EGL_VENDOR_LIBRARY_FILENAMES=$3;VK_LOADER_DRIVERS_DISABLE=$4;once tw is up=$5;$a11y;$told;"
	shift 5
	got=$(session "$@")
	[ "$got" = "$want" ] || fail "tw-session, $description: $got"
}

machine
expect_session "nothing on the NVIDIA GPU" /dev/dri/card1 "$mesa_egl" "$mesa_vulkan" "$step"
# Many laptops call the second GPU a 3D controller, which has no boot_vga.
echo 0x030200 >"$nvidia/class"
rm "$nvidia/boot_vga"
expect_session "the NVIDIA GPU as a 3D controller" /dev/dri/card1 "$mesa_egl" "$mesa_vulkan" "$step"
"$bin/driver" unload
expect_session "a 3D controller without its driver" /dev/dri/card1 "$mesa_egl" "$mesa_vulkan" "$step"
"$bin/driver" load
echo 0x030000 >"$nvidia/class"
echo 0 >"$nvidia/boot_vga"
# Its driver unloaded: there is no second card, and tw must still be told to
# use the first one only, or it would open the second when the driver loads.
"$bin/driver" unload
expect_session "the NVIDIA driver unloaded" /dev/dri/card1 "$mesa_egl" "$mesa_vulkan" "$step"
# Switched off by tw-gpu, as after logging out and in: not even on the bus.
echo "$port" >"$flag"
"$bin/pci" remove
expect_session "the NVIDIA GPU switched off" /dev/dri/card1 "$mesa_egl" "$mesa_vulkan" "$step"
"$bin/pci" rescan
rm "$flag"
# A monitor on the NVIDIA GPU: tw uses both, and the GPU stays on.
: >"$tmp/hdmi"
"$bin/driver" load
expect_session "a monitor on the NVIDIA GPU" /dev/dri/card1:/dev/dri/card0 '' '' ''
rm "$tmp/hdmi"
"$bin/driver" load
expect_session "all GPUs asked for" '' '' '' '' TW_ALL_GPUS=1
expect_session "GPUs named by the user" /dev/dri/card0 '' '' '' WLR_DRM_DEVICES=/dev/dri/card0
# The NVIDIA GPU has the panel (a desktop): nothing of NVIDIA's is kept out.
echo 0 >"$intel/boot_vga"
echo 1 >"$nvidia/boot_vga"
echo disconnected >"$sys/class/drm/card1-eDP-1/status"
expect_session "the NVIDIA GPU as the first one" /dev/dri/card0 '' '' ''
# One GPU: nothing to decide.
machine
"$bin/driver" unload
rm -rf "$nvidia" "$bus/0000:01:00.0"
expect_session "a single GPU" '' '' '' ''
a11y='NO_AT_BRIDGE= GTK_A11Y='
told_none='told to services=WAYLAND_DISPLAY XDG_CURRENT_DESKTOP;taken back= NO_AT_BRIDGE GTK_A11Y __EGL_VENDOR_LIBRARY_FILENAMES VK_LOADER_DRIVERS_DISABLE'
expect_session "the accessibility bus asked for" '' '' '' '' TW_A11Y=1

# --- tw-session: what the services are told once tw is up ---------------------------
# A stand-in tw runs the startup command it is handed, the way tw does, with
# a stand-in for the program that passes the environment on to D-Bus and
# systemd and nothing else to find: no tw-gpu, no autostart script.
mkdir -p "$tmp/session" "$tmp/session-bin"
cp "$data/tw-session" "$tmp/session/tw-session"
cat >"$tmp/session/tw" <<EOF
#!/bin/sh
[ ! -e '$tmp/no-library' ] || exit 127 # what the loader leaves when a library is missing
[ "\$1" = -h ] && exit 0
[ "\$1" = -s ] && WAYLAND_DISPLAY=wayland-9 PATH='$tmp/session-bin' exec /bin/sh -c "\$2"
EOF
cat >"$tmp/session-bin/dbus-update-activation-environment" <<EOF
#!/bin/sh
echo "\$* NO_AT_BRIDGE=\${NO_AT_BRIDGE-} GTK_A11Y=\${GTK_A11Y-}" >'$tmp/told'
EOF
cat >"$tmp/session-bin/systemctl" <<EOF
#!/bin/sh
echo "\$*" >>'$tmp/took-back'
EOF
chmod +x "$tmp/session/tw" "$tmp/session-bin/dbus-update-activation-environment" \
	"$tmp/session-bin/systemctl"
told() { # [VAR=value...]: what a session started like this passes on
	rm -f "$tmp/told" "$tmp/took-back"
	env -i PATH="$PATH" HOME="$tmp/home" TW_SYS="$tmp/nothing" TW_RUN="$tmp/nothing" "$@" \
		sh "$tmp/session/tw-session" >/dev/null 2>&1
	cat "$tmp/told" 2>/dev/null
}
got=$(told)
[ "$got" = "--systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP NO_AT_BRIDGE GTK_A11Y NO_AT_BRIDGE=1 GTK_A11Y=none" ] ||
	fail "tw-session did not tell the services to leave the accessibility bus alone: $got"
[ "$(cat "$tmp/took-back" 2>/dev/null)" = "--user unset-environment __EGL_VENDOR_LIBRARY_FILENAMES VK_LOADER_DRIVERS_DISABLE" ] ||
	fail "tw-session on one GPU did not take back the GPU an earlier session named: $(cat "$tmp/took-back" 2>/dev/null)"
got=$(told TW_A11Y=1)
[ "$got" = "--systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP NO_AT_BRIDGE= GTK_A11Y=" ] ||
	fail "tw-session with TW_A11Y=1 still spoke of the accessibility bus: $got"
# ... and takes back what an earlier session told systemd, which the bus's
# own unit goes by.
[ "$(cat "$tmp/took-back" 2>/dev/null)" = "--user unset-environment NO_AT_BRIDGE GTK_A11Y __EGL_VENDOR_LIBRARY_FILENAMES VK_LOADER_DRIVERS_DISABLE" ] ||
	fail "tw-session with TW_A11Y=1 left systemd told to keep the bus off: $(cat "$tmp/took-back" 2>/dev/null)"
got=$(told DISPLAY=:7)
[ "$got" = "--systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP NO_AT_BRIDGE GTK_A11Y NO_AT_BRIDGE=1 GTK_A11Y=none" ] ||
	fail "tw-session passed on a DISPLAY that is not tw's: $got"

# A tw that cannot start for want of a library, as after a system update:
# the session says how to build it again, and only then.
session_says() { # what tw-session prints to the console
	env -i PATH="$PATH" HOME="$tmp/home" TW_SYS="$tmp/nothing" TW_RUN="$tmp/nothing" \
		sh "$tmp/session/tw-session" 2>&1 >/dev/null
}
[ -z "$(session_says)" ] || fail "tw-session had something to say although tw started: $(session_says)"
: >"$tmp/no-library"
session_says | grep -q 'meson setup --wipe build-release' ||
	fail "tw-session did not say how to build a tw again that cannot start: $(session_says)"
rm "$tmp/no-library"

# The accessibility bus's unit starts only where systemd has not been told
# the variable that tw-session tells it: the two have to name the same one,
# and the condition has to read the way it is meant.
grep -q '^ConditionEnvironment=!NO_AT_BRIDGE$' "$data/tw-a11y.conf" ||
	fail "the line added to the accessibility bus's unit does not go by NO_AT_BRIDGE"
if command -v systemd-analyze >/dev/null 2>&1; then
	condition=$(grep '^Condition' "$data/tw-a11y.conf")
	if env NO_AT_BRIDGE=1 systemd-analyze condition "$condition" >/dev/null 2>&1; then
		fail "the accessibility bus would start in a session that keeps it off"
	fi
	env -u NO_AT_BRIDGE systemd-analyze condition "$condition" >/dev/null 2>&1 ||
		fail "the accessibility bus would not start in a session that asked for it"
fi

echo "ok: tw-gpu run, launch, on, off, login and status; the root helper; the GPU choice, the accessibility bus and a tw that cannot start in tw-session"
