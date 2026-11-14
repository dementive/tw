#!/bin/sh
# tw-gpu, its root helper and the GPU choice in tw-session, run against a
# made-up machine: directories standing in for /sys and /dev, and stand-ins
# for systemctl and modprobe that load and unload a pretend driver. Nothing
# here touches a real GPU or driver, or needs root. What it cannot show is
# that the real driver unloads, that the GPU then powers down, or that a
# program draws with it afterwards.
# $1 = the data/ directory of the source tree.
set -eu
data=$1
tmp=$(mktemp -d "$PWD/gpu-test.XXXXXX")
trap 'pkill -f "$data/tw-gpu settle" 2>/dev/null || true; rm -rf "$tmp"' EXIT
sys=$tmp/sys
dev=$tmp/dev
bin=$tmp/bin        # stand-ins for tw-gpu: systemctl
rootbin=$tmp/rootbin # stand-ins for tw-gpu-root: systemctl, modprobe
log=$tmp/log
mkdir -p "$bin" "$rootbin" "$tmp/run" "$tmp/home"
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
calls() { grep -c "$1" "$log" || true; }                  # log lines matching
loaded() { [ -d "$sys/module/nvidia" ]; }
settled() { ! pgrep -f "$data/tw-gpu settle" >/dev/null; } # no switch-off pending
last_report() { tail -n 1 "$tmp/report"; }
expect_report() { # <description> <regex>
	last_report | grep -Eq "$2" || fail "$1: $(last_report)"
}

# --- The made-up machine ------------------------------------------------------
# A laptop: an Intel GPU with the built-in panel, and an NVIDIA GPU with an
# HDMI port and a sound function, its driver loaded.
nvidia=$sys/bus/pci/devices/0000:01:00.0
machine() {
	rm -rf "$sys" "$dev"
	pci=$sys/bus/pci/devices
	mkdir -p "$sys/class/drm" "$sys/module" "$dev/dri" \
		"$sys/bus/pci/drivers/i915" "$sys/bus/pci/drivers/nvidia" \
		"$pci/0000:00:02.0/drm/card1" "$pci/0000:00:02.0/drm/renderD128" \
		"$sys/class/drm/card1-eDP-1" "$nvidia/power" "$pci/0000:01:00.1"
	echo 0x8086 >"$pci/0000:00:02.0/vendor"
	echo 0x030000 >"$pci/0000:00:02.0/class"
	echo 1 >"$pci/0000:00:02.0/boot_vga"
	ln -s ../../drivers/i915 "$pci/0000:00:02.0/driver"
	ln -s "$pci/0000:00:02.0" "$pci/0000:00:02.0/drm/card1/device"
	ln -s "$pci/0000:00:02.0/drm/card1" "$sys/class/drm/card1"
	echo connected >"$sys/class/drm/card1-eDP-1/status"
	: >"$dev/dri/card1"
	: >"$dev/dri/renderD128"
	echo 0x10de >"$nvidia/vendor"
	echo 0x030000 >"$nvidia/class"
	echo 0 >"$nvidia/boot_vga"
	echo D0 >"$nvidia/power_state"
	echo 0x10de >"$pci/0000:01:00.1/vendor" # the sound function: not a GPU
	echo 0x040300 >"$pci/0000:01:00.1/class"
	"$bin/driver" load
}

# driver load|unload: what the real driver does to /sys and /dev.
cat >"$bin/driver" <<EOF
#!/bin/sh
case \$1 in
load)
	mkdir -p "$sys/module/nvidia" "$sys/module/nvidia_modeset" "$sys/module/nvidia_uvm" \\
		"$sys/module/nvidia_drm" "$nvidia/drm/card0" "$nvidia/drm/renderD129" \\
		"$sys/class/drm/card0-HDMI-A-1"
	ln -sfn ../../drivers/nvidia "$nvidia/driver"
	ln -sfn "$nvidia" "$nvidia/drm/card0/device"
	ln -sfn "$nvidia/drm/card0" "$sys/class/drm/card0"
	[ -e "$tmp/hdmi" ] && status=connected || status=disconnected
	echo \$status >"$sys/class/drm/card0-HDMI-A-1/status"
	: >"$dev/dri/card0"
	: >"$dev/dri/renderD129"
	echo suspended >"$nvidia/power/runtime_status"
	echo D3cold >"$nvidia/power_state"
	;;
unload)
	rm -rf "$sys"/module/nvidia* "$nvidia/drm" "$nvidia/driver" "$sys/class/drm/card0" \\
		"$sys/class/drm/card0-HDMI-A-1" "$dev/dri/card0" "$dev/dri/renderD129"
	;;
esac
EOF
# What tw-gpu reaches the root helper through.
cat >"$bin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >>"$log"
case "\$*" in
"start tw-gpu@on.service")
	[ -e "$tmp/on-fails" ] && exit 1
	"$bin/driver" load
	;;
"start tw-gpu@off.service")
	[ -e "$tmp/busy" ] || "$bin/driver" unload
	;;
esac
EOF
# A program that says what it was started with.
cat >"$bin/report" <<EOF
#!/bin/sh
echo "offload=\${__NV_PRIME_RENDER_OFFLOAD-} vulkan=\${__VK_LAYER_NV_optimus-}" \\
	"glx=\${__GLX_VENDOR_LIBRARY_NAME-} egl=\${__EGL_VENDOR_LIBRARY_FILENAMES-}" \\
	"icd=\${VK_DRIVER_FILES-} driver=\$([ -d "$sys/module/nvidia" ] && echo loaded || echo absent)" \\
	"args=\$*" >>"$tmp/report"
EOF
chmod +x "$bin/driver" "$bin/systemctl" "$bin/report"

# tw-gpu as a program in the session would run it: the session's two
# variables that keep programs on the Intel GPU are set.
gpu() {
	env PATH="$bin:$PATH" TW_SYS="$sys" TW_DEV="$dev" HOME="$tmp/home" \
		XDG_RUNTIME_DIR="$tmp/run" XDG_DATA_DIRS="$tmp/share" \
		WLR_DRM_DEVICES=/dev/dri/card1 \
		__EGL_VENDOR_LIBRARY_FILENAMES=/mesa.json VK_DRIVER_FILES=/intel.json \
		"$@"
}
on_nvidia='offload=1 vulkan=NVIDIA_only glx=nvidia egl= icd= driver=loaded'
on_intel='offload= vulkan= glx= egl=/mesa.json icd=/intel.json'

# --- tw-gpu: status, off, on --------------------------------------------------
machine
[ "$(gpu "$data/tw-gpu" status)" = "NVIDIA GPU 0000:01:00.0: on, asleep" ] ||
	fail "status with the driver loaded: $(gpu "$data/tw-gpu" status)"
gpu "$data/tw-gpu" off || fail "off failed on an idle GPU"
loaded && fail "off left the driver loaded"
[ "$(calls 'start tw-gpu@off')" = 1 ] || fail "off did not start the unit once"
echo D3cold >"$nvidia/power_state"
[ "$(gpu "$data/tw-gpu" status)" = "NVIDIA GPU 0000:01:00.0: off, powered down" ] ||
	fail "status with the driver unloaded: $(gpu "$data/tw-gpu" status)"
echo D0 >"$nvidia/power_state"
gpu "$data/tw-gpu" status | grep -q 'off, but still powered (D0)' ||
	fail "status does not say that the GPU failed to power down"
gpu "$data/tw-gpu" off || fail "off failed with the GPU already off"
[ "$(calls 'start tw-gpu@off')" = 1 ] || fail "off did something with the GPU already off"

# --- run: on for the program, off after it ------------------------------------
gpu "$data/tw-gpu" run report a 'b c' || fail "run failed"
expect_report "a program started with run" "^$on_nvidia args=a b c\$"
[ "$(calls 'start tw-gpu@on')" = 1 ] || fail "run did not switch the GPU on once"
wait_until "the GPU to go off after the program" settled
loaded && fail "the GPU stayed on after its only program exited"
status=0
gpu "$data/tw-gpu" run sh -c 'exit 7' || status=$?
[ "$status" = 7 ] || fail "run returned $status for a program that exited with 7"
wait_until "the switch-off to finish" settled

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
loaded && fail "the GPU stayed on after both programs exited"
[ "$(calls 'start tw-gpu@off')" = $((before + 1)) ] || fail "expected one switch-off for two programs"

# A program that leaves something running and returns: run returns with it,
# and the GPU stays on for what was left behind.
gpu "$data/tw-gpu" run sh -c 'sleep 1.5 &'
loaded || fail "the GPU went off under a program left in the background"
sleep 0.5
loaded || fail "the GPU went off under a program left in the background"
wait_until "the GPU to go off after the background program" settled
loaded && fail "the GPU stayed on after the background program exited"

# --- on: stays on until off -----------------------------------------------------
gpu "$data/tw-gpu" on || fail "on failed"
loaded || fail "on did not load the driver"
gpu "$data/tw-gpu" status | grep -q "kept on by 'tw-gpu on'" || fail "status does not say why it is on"
gpu "$data/tw-gpu" run report kept
wait_until "the switch-off check to finish" settled
loaded || fail "a program exiting switched off a GPU that was asked to stay on"
gpu "$data/tw-gpu" off || fail "off failed after on"
loaded && fail "off after on left the driver loaded"

# --- A GPU in use stays on -------------------------------------------------------
gpu "$data/tw-gpu" on
: >"$tmp/busy"
if gpu "$data/tw-gpu" off 2>"$tmp/err"; then fail "off claimed success on a busy GPU"; fi
grep -q 'in use' "$tmp/err" || fail "off did not say the GPU is in use"
loaded || fail "the driver went although the GPU was busy"
rm "$tmp/busy"
# One that tw drives a monitor with is not even tried.
before=$(calls 'start tw-gpu@off')
if gpu env WLR_DRM_DEVICES=/dev/dri/card1:/dev/dri/card0 "$data/tw-gpu" off 2>/dev/null; then
	fail "off succeeded on the GPU driving a monitor"
fi
[ "$(calls 'start tw-gpu@off')" = "$before" ] || fail "tried to unload the driver of a GPU driving a monitor"
gpu env WLR_DRM_DEVICES=/dev/dri/card1:/dev/dri/card0 "$data/tw-gpu" status |
	grep -q 'drives a monitor' || fail "status does not mention the monitor"
gpu "$data/tw-gpu" off || fail "off failed once the GPU was free"

# --- The GPU cannot be switched on: the program still runs -----------------------
: >"$tmp/on-fails"
status=0
gpu "$data/tw-gpu" run sh -c 'report fallback; exit 3' 2>"$tmp/err" || status=$?
[ "$status" = 3 ] || fail "run returned $status when the GPU could not be switched on"
expect_report "a program whose GPU could not be switched on" "^$on_intel driver=absent args=fallback\$"
grep -q 'could not switch' "$tmp/err" || fail "run did not say the GPU could not be switched on"
if gpu "$data/tw-gpu" on 2>/dev/null; then fail "on claimed success"; fi
rm "$tmp/on-fails" "$tmp/run/tw-gpu.keep"
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
loaded && fail "the GPU stayed on after the launched program exited"
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
rm -rf "$nvidia"
before=$(calls systemctl)
gpu "$data/tw-gpu" run report plain
expect_report "run without an NVIDIA GPU" "^$on_intel driver=absent args=plain\$"
gpu env DESKTOP_ENTRY_ID=game.desktop "$data/tw-gpu" launch report plain-launch
expect_report "launch without an NVIDIA GPU" "^$on_intel driver=absent args=plain-launch\$"
[ "$(gpu "$data/tw-gpu" status)" = "no NVIDIA GPU" ] || fail "status without an NVIDIA GPU"
if gpu "$data/tw-gpu" on 2>/dev/null; then fail "on succeeded without an NVIDIA GPU"; fi
gpu "$data/tw-gpu" off || fail "off failed without an NVIDIA GPU"
[ "$(calls systemctl)" = "$before" ] || fail "something was started on a machine with no NVIDIA GPU"

# --- The root helper ---------------------------------------------------------------
cat >"$rootbin/modprobe" <<EOF
#!/bin/sh
echo "modprobe \$*" >>"$log"
case "\$*" in
nvidia_drm) "$bin/driver" load ;;
"-r nvidia_drm")
	[ -e "$tmp/drawing" ] && exit 1
	rm -rf "$sys/module/nvidia_drm"
	;;
"-r nvidia_uvm nvidia_modeset nvidia")
	[ -e "$tmp/computing" ] && exit 1
	"$bin/driver" unload
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
chmod +x "$rootbin/modprobe" "$rootbin/systemctl"
root() { # tw-gpu-root <on|off>, then what it ran, on one line
	: >"$log"
	env PATH="$rootbin:$PATH" TW_SYS="$sys" "$data/tw-gpu-root" "$@" 2>"$tmp/err" || return
	ran=$(tr '\n' ';' <"$log")
}
powerd_start='systemctl is-enabled --quiet nvidia-powerd.service;systemctl reset-failed nvidia-powerd.service;systemctl start --no-block nvidia-powerd.service;'

machine
"$bin/driver" unload
root on || fail "the helper failed to load the driver"
[ "$ran" = "modprobe nvidia_drm;$powerd_start" ] || fail "helper on ran: $ran"
loaded || fail "helper on did not load the driver"

root off || fail "the helper failed to unload the driver"
[ "$ran" = "modprobe -r nvidia_drm;systemctl stop nvidia-powerd.service;modprobe -r nvidia_uvm nvidia_modeset nvidia;" ] ||
	fail "helper off ran: $ran"
loaded && fail "helper off left the driver loaded"
root off || fail "the helper failed with the driver already unloaded"
[ -z "$ran" ] || fail "helper off with nothing loaded ran: $ran"

# A program is drawing with the GPU: nothing is disturbed, and that is not an
# error.
root on
: >"$tmp/drawing"
root off || fail "the helper reported an error for a GPU in use"
[ "$ran" = "modprobe -r nvidia_drm;modprobe -r nvidia_drm;modprobe -r nvidia_drm;modprobe -r nvidia_drm;modprobe -r nvidia_drm;" ] ||
	fail "helper off on a GPU in use ran: $ran"
[ -d "$sys/module/nvidia_drm" ] || fail "the helper took the driver from a program drawing with it"
grep -q 'in use' "$tmp/err" || fail "the helper did not say the GPU is in use"
rm "$tmp/drawing"

# A program is computing with it: what was unloaded is put back.
: >"$tmp/computing"
root off || fail "the helper reported an error for a GPU computing"
[ "$ran" = "modprobe -r nvidia_drm;systemctl stop nvidia-powerd.service;modprobe -r nvidia_uvm nvidia_modeset nvidia;modprobe nvidia_drm;$powerd_start" ] ||
	fail "helper off on a GPU computing ran: $ran"
[ -d "$sys/module/nvidia_drm" ] || fail "the helper did not put the driver back"
rm "$tmp/computing"

# The power daemon is left alone if the owner switched it off.
: >"$tmp/powerd-disabled"
root off
root on
[ "$ran" = "modprobe nvidia_drm;systemctl is-enabled --quiet nvidia-powerd.service;" ] ||
	fail "helper on with the power daemon disabled ran: $ran"
rm "$tmp/powerd-disabled"

status=0
root sideways || status=$?
[ "$status" = 2 ] && [ ! -s "$log" ] || fail "the helper acted on a word it does not know"

# --- tw-session: which GPUs tw uses -------------------------------------------------
mkdir -p "$tmp/usr/glvnd/egl_vendor.d" "$tmp/usr/vulkan/icd.d"
for file in glvnd/egl_vendor.d/10_nvidia.json glvnd/egl_vendor.d/50_mesa.json \
	vulkan/icd.d/intel_icd.json vulkan/icd.d/intel_hasvk_icd.json vulkan/icd.d/nvidia_icd.json; do
	echo '{}' >"$tmp/usr/$file"
done
mesa_egl=$tmp/usr/glvnd/egl_vendor.d/50_mesa.json
mesa_vulkan=$tmp/usr/vulkan/icd.d/intel_hasvk_icd.json:$tmp/usr/vulkan/icd.d/intel_icd.json
session() { # [VAR=value...]: what tw-session decides, on one line
	env -i PATH="$PATH" HOME="$tmp/home" TW_SYS="$sys" TW_SHARE="$tmp/usr" TW_SESSION_PRINT=1 \
		"$@" sh "$data/tw-session" | tr '\n' ';'
}
expect_session() { # <description> <devices> <egl> <vulkan> <off> [VAR=value...]
	description=$1
	want="WLR_DRM_DEVICES=$2;__EGL_VENDOR_LIBRARY_FILENAMES=$3;VK_DRIVER_FILES=$4;switch the second GPU off=$5;"
	shift 5
	got=$(session "$@")
	[ "$got" = "$want" ] || fail "tw-session, $description: $got"
}

machine
expect_session "nothing on the NVIDIA GPU" /dev/dri/card1 "$mesa_egl" "$mesa_vulkan" yes
# Its driver unloaded: there is no second card, and tw must still be told to
# use the first one only, or it would open the second when the driver loads.
"$bin/driver" unload
expect_session "the NVIDIA driver unloaded" /dev/dri/card1 "$mesa_egl" "$mesa_vulkan" yes
# A monitor on the NVIDIA GPU: tw uses both, and the GPU stays on.
: >"$tmp/hdmi"
"$bin/driver" load
expect_session "a monitor on the NVIDIA GPU" /dev/dri/card1:/dev/dri/card0 '' '' ''
rm "$tmp/hdmi"
"$bin/driver" load
expect_session "all GPUs asked for" '' '' '' '' TW_ALL_GPUS=1
expect_session "GPUs named by the user" /dev/dri/card0 '' '' '' WLR_DRM_DEVICES=/dev/dri/card0
# The NVIDIA GPU has the panel (a desktop): nothing of NVIDIA's is kept out.
echo 0 >"$sys/bus/pci/devices/0000:00:02.0/boot_vga"
echo 1 >"$nvidia/boot_vga"
echo disconnected >"$sys/class/drm/card1-eDP-1/status"
expect_session "the NVIDIA GPU as the first one" /dev/dri/card0 '' '' ''
# One GPU: nothing to decide.
machine
rm -rf "$nvidia" "$sys/class/drm/card0" "$sys/class/drm/card0-HDMI-A-1" "$sys"/module/nvidia*
expect_session "a single GPU" '' '' '' ''

echo "ok: tw-gpu run, launch, on, off and status; the root helper; the GPU choice in tw-session"
